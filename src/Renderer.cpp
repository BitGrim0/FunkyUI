// D3D11 + DirectComposition renderer: every shape is one instanced quad whose signed distance
// function is evaluated in Shaders/Shape.hlsl. No input layout, no vertex buffer.
// Embedded mode draws with the client's device into the client's render target instead.

#include "Internal.h"

#include <new>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>

#include "ShapeVS.h"
#include "ShapePS.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

namespace Funky
{
    namespace
    {
        constexpr uint32_t RestoreInterval = 60; // frames between attempts to recreate a lost device

        template <class T>
        void SafeRelease(T*& object)
        {
            if (object)
            {
                object->Release();
                object = nullptr;
            }
        }

        // Dynamic structured buffer with its view, grown by doubling.
        struct GpuBuffer
        {
            ID3D11Buffer* Buffer;
            ID3D11ShaderResourceView* View;
            uint32_t Capacity; // elements

            void Release()
            {
                SafeRelease(View);
                SafeRelease(Buffer);
                Capacity = 0;
            }

            bool Upload(ID3D11Device* device, ID3D11DeviceContext* context, const void* data, uint32_t count, uint32_t stride)
            {
                if (count > Capacity)
                {
                    uint32_t capacity = Capacity ? Capacity : 256;
                    while (capacity < count)
                        capacity *= 2;
                    Release();

                    D3D11_BUFFER_DESC desc = {};
                    desc.ByteWidth = capacity * stride;
                    desc.Usage = D3D11_USAGE_DYNAMIC;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                    desc.StructureByteStride = stride;

                    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
                    viewDesc.Format = DXGI_FORMAT_UNKNOWN;
                    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                    viewDesc.Buffer.NumElements = capacity;

                    if (FAILED(device->CreateBuffer(&desc, nullptr, &Buffer)) ||
                        FAILED(device->CreateShaderResourceView(Buffer, &viewDesc, &View)))
                        return false;
                    Capacity = capacity;
                }

                D3D11_MAPPED_SUBRESOURCE mapped;
                if (FAILED(context->Map(Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                    return false;
                MemCopy(mapped.pData, data, size_t(count) * stride);
                context->Unmap(Buffer, 0);
                return true;
            }
        };

        // Mirrors cbuffer Constants in Shape.hlsl.
        struct ShaderConstants
        {
            float ViewportSize[2];      // DIPs
            float DpiScale;
            uint32_t FirstShape;        // SV_InstanceID does not include the draw's start instance
            float AtlasTexelSize[2];
            float Padding[2];
        };

        static_assert(sizeof(ShaderConstants) % 16 == 0);

        // A shader stage's shader and its class instances (dynamic linkage), as the Get call returns them.
        template <class T>
        struct SavedShader
        {
            T* Shader;
            ID3D11ClassInstance* Instances[D3D11_SHADER_MAX_INTERFACES];
            UINT InstanceCount;

            void Release()
            {
                SafeRelease(Shader);
                for (UINT i = 0; i < InstanceCount; ++i)
                    SafeRelease(Instances[i]);
            }
        };

        // The client's device context state that an embedded Render changes, like imgui_impl_dx11.cpp saves it.
        // Left alone, so not saved: vertex / index buffers (no input layout), scissor rects (scissor test off)
        // and the depth-stencil state (no depth-stencil view is bound while drawing, so the tests are off).
        struct ContextState
        {
            ID3D11RenderTargetView* Targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
            ID3D11DepthStencilView* DepthStencil;
            ID3D11BlendState* Blend;
            float BlendFactor[4];
            UINT SampleMask;
            ID3D11RasterizerState* Rasterizer;
            D3D11_VIEWPORT Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            UINT ViewportCount;
            ID3D11InputLayout* InputLayout;
            D3D11_PRIMITIVE_TOPOLOGY Topology;
            SavedShader<ID3D11VertexShader> VS;
            SavedShader<ID3D11PixelShader> PS;
            SavedShader<ID3D11GeometryShader> GS; // the other stages are unbound while drawing
            SavedShader<ID3D11HullShader> HS;
            SavedShader<ID3D11DomainShader> DS;
            ID3D11ShaderResourceView* VSView;
            ID3D11ShaderResourceView* PSViews[4];
            ID3D11SamplerState* PSSampler;
            ID3D11Buffer* VSConstants;
            ID3D11Buffer* PSConstants;

            void Save(ID3D11DeviceContext* c)
            {
                c->OMGetRenderTargets(ARRAYSIZE(Targets), Targets, &DepthStencil);
                c->OMGetBlendState(&Blend, BlendFactor, &SampleMask);
                c->RSGetState(&Rasterizer);
                ViewportCount = ARRAYSIZE(Viewports);
                c->RSGetViewports(&ViewportCount, Viewports);
                c->IAGetInputLayout(&InputLayout);
                c->IAGetPrimitiveTopology(&Topology);
                VS.InstanceCount = PS.InstanceCount = GS.InstanceCount = HS.InstanceCount = DS.InstanceCount = D3D11_SHADER_MAX_INTERFACES;
                c->VSGetShader(&VS.Shader, VS.Instances, &VS.InstanceCount);
                c->PSGetShader(&PS.Shader, PS.Instances, &PS.InstanceCount);
                c->GSGetShader(&GS.Shader, GS.Instances, &GS.InstanceCount);
                c->HSGetShader(&HS.Shader, HS.Instances, &HS.InstanceCount);
                c->DSGetShader(&DS.Shader, DS.Instances, &DS.InstanceCount);
                c->VSGetShaderResources(0, 1, &VSView);
                c->PSGetShaderResources(0, ARRAYSIZE(PSViews), PSViews);
                c->PSGetSamplers(0, 1, &PSSampler);
                c->VSGetConstantBuffers(0, 1, &VSConstants);
                c->PSGetConstantBuffers(0, 1, &PSConstants);
            }

            // Render targets first: a shader view of a resource still bound as a target would be unbound again.
            void Restore(ID3D11DeviceContext* c)
            {
                // Only up to the last bound target: UAVs in the slots after it stay as they are.
                UINT targetCount = ARRAYSIZE(Targets);
                while (targetCount > 0 && !Targets[targetCount - 1])
                    --targetCount;
                c->OMSetRenderTargets(targetCount, Targets, DepthStencil);
                c->OMSetBlendState(Blend, BlendFactor, SampleMask);
                c->RSSetState(Rasterizer);
                c->RSSetViewports(ViewportCount, Viewports);
                c->IASetInputLayout(InputLayout);
                c->IASetPrimitiveTopology(Topology);
                c->VSSetShader(VS.Shader, VS.Instances, VS.InstanceCount);
                c->PSSetShader(PS.Shader, PS.Instances, PS.InstanceCount);
                c->GSSetShader(GS.Shader, GS.Instances, GS.InstanceCount);
                c->HSSetShader(HS.Shader, HS.Instances, HS.InstanceCount);
                c->DSSetShader(DS.Shader, DS.Instances, DS.InstanceCount);
                c->VSSetShaderResources(0, 1, &VSView);
                c->PSSetShaderResources(0, ARRAYSIZE(PSViews), PSViews);
                c->PSSetSamplers(0, 1, &PSSampler);
                c->VSSetConstantBuffers(0, 1, &VSConstants);
                c->PSSetConstantBuffers(0, 1, &PSConstants);

                for (ID3D11RenderTargetView*& target : Targets)
                    SafeRelease(target);
                SafeRelease(DepthStencil);
                SafeRelease(Blend);
                SafeRelease(Rasterizer);
                SafeRelease(InputLayout);
                VS.Release();
                PS.Release();
                GS.Release();
                HS.Release();
                DS.Release();
                SafeRelease(VSView);
                for (ID3D11ShaderResourceView*& view : PSViews)
                    SafeRelease(view);
                SafeRelease(PSSampler);
                SafeRelease(VSConstants);
                SafeRelease(PSConstants);
            }
        };
    }

    struct Renderer::Impl
    {
        bool Embedded;      // the client's device: no swap chain, no device recovery, its context state is restored
        HWND Window;
        ID3D11Device* Device;
        ID3D11DeviceContext* Context;
        IDXGISwapChain2* SwapChain;
        HANDLE FrameWaitable;
        ID3D11RenderTargetView* BackBuffer;
        IDCompositionDevice* Composition;
        IDCompositionTarget* Target;
        IDCompositionVisual* Visual;
        ID3D11RenderTargetView* ClientTarget; // embedded: the view the next Render draws into
        ContextState Saved;                   // embedded: the client's state during Render (too big for the stack)

        ID3D11VertexShader* VertexShader;
        ID3D11PixelShader* PixelShader;
        ID3D11BlendState* Blend;
        ID3D11RasterizerState* Rasterizer;
        ID3D11SamplerState* Sampler;
        ID3D11Buffer* Constants;
        GpuBuffer Shapes;
        GpuBuffer Clips;
        GpuBuffer Points;

        ID3D11Texture2D* Atlas;
        ID3D11ShaderResourceView* AtlasView;
        uint32_t AtlasSize;

        uint32_t Width;     // physical pixels, as last requested (a restored device uses them)
        uint32_t Height;
        uint32_t RestoreDelay; // frames until the next attempt to recreate a lost device

        // Embedded: the device is already set. Until the first glyph arrives, t2 samples a 1x1 empty atlas.
        bool CreateAll()
        {
            const uint8_t emptyTexel[2] = {};
            return (Embedded || (CreateDevice() && CreateSwapChain())) && CreatePipeline() && CreateAtlas(1, emptyTexel);
        }

        // Leaves every object null, ready for CreateAll again.
        void ReleaseAll()
        {
            if (Context && !Embedded) // the client's context keeps its state
            {
                Context->ClearState();
                Context->Flush(); // deferred destruction: the old swap chain and targets go away now
            }
            SafeRelease(ClientTarget);
            SafeRelease(Visual);
            SafeRelease(Target);
            SafeRelease(Composition);
            SafeRelease(BackBuffer);
            if (FrameWaitable)
            {
                CloseHandle(FrameWaitable);
                FrameWaitable = nullptr;
            }
            SafeRelease(SwapChain);

            Shapes.Release();
            Clips.Release();
            Points.Release();
            SafeRelease(AtlasView);
            SafeRelease(Atlas);
            AtlasSize = 0;
            SafeRelease(Constants);
            SafeRelease(Sampler);
            SafeRelease(Rasterizer);
            SafeRelease(Blend);
            SafeRelease(PixelShader);
            SafeRelease(VertexShader);
            SafeRelease(Context);
            SafeRelease(Device);
        }

        bool CreateDevice()
        {
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            HRESULT result = E_FAIL;
#if defined(_DEBUG)
            // The debug layer exists only where the Graphics Tools feature is installed.
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG,
                                       levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &Device, nullptr, &Context);
#endif
            if (FAILED(result))
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                           levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &Device, nullptr, &Context);
            return SUCCEEDED(result);
        }

        // Composition swap chain and the DirectComposition tree that shows it in the window.
        bool CreateSwapChain()
        {
            DXGI_SWAP_CHAIN_DESC1 desc = {};
            desc.Width = Width;
            desc.Height = Height;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

            IDXGIDevice* dxgiDevice = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory2* factory = nullptr;
            IDXGISwapChain1* swapChain = nullptr;
            bool ok = SUCCEEDED(Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
                      SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
                      SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) &&
                      SUCCEEDED(factory->CreateSwapChainForComposition(Device, &desc, nullptr, &swapChain)) &&
                      SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&SwapChain))) &&
                      SUCCEEDED(SwapChain->SetMaximumFrameLatency(1)) &&
                      SUCCEEDED(DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&Composition))) &&
                      SUCCEEDED(Composition->CreateTargetForHwnd(Window, TRUE, &Target)) &&
                      SUCCEEDED(Composition->CreateVisual(&Visual)) &&
                      SUCCEEDED(Visual->SetContent(SwapChain)) &&
                      SUCCEEDED(Target->SetRoot(Visual)) &&
                      SUCCEEDED(Composition->Commit());
            SafeRelease(swapChain);
            SafeRelease(factory);
            SafeRelease(adapter);
            SafeRelease(dxgiDevice);
            if (!ok)
                return false;

            // The waitable starts signaled: take that count now, so every later wait pairs with a Present.
            FrameWaitable = SwapChain->GetFrameLatencyWaitableObject();
            if (!FrameWaitable)
                return false;
            WaitForSingleObjectEx(FrameWaitable, 1000, TRUE);
            return CreateBackBufferView();
        }

        // With the flip model in D3D11, buffer 0 is always the current back buffer: one view is enough.
        bool CreateBackBufferView()
        {
            ID3D11Texture2D* buffer = nullptr;
            bool ok = SUCCEEDED(SwapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) &&
                      SUCCEEDED(Device->CreateRenderTargetView(buffer, nullptr, &BackBuffer));
            SafeRelease(buffer);
            return ok;
        }

        bool CreatePipeline()
        {
            // Premultiplied alpha, blended in sRGB space like DirectComposition does.
            D3D11_BLEND_DESC blend = {};
            D3D11_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D11_BLEND_ONE;
            target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            target.BlendOp = D3D11_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D11_BLEND_ONE;
            target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

            D3D11_RASTERIZER_DESC rasterizer = {};
            rasterizer.FillMode = D3D11_FILL_SOLID;
            rasterizer.CullMode = D3D11_CULL_NONE;
            rasterizer.DepthClipEnable = TRUE;

            D3D11_SAMPLER_DESC sampler = {};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;

            D3D11_BUFFER_DESC constants = {};
            constants.ByteWidth = sizeof(ShaderConstants);
            constants.Usage = D3D11_USAGE_DYNAMIC;
            constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            return SUCCEEDED(Device->CreateVertexShader(g_ShapeVS, sizeof(g_ShapeVS), nullptr, &VertexShader)) &&
                   SUCCEEDED(Device->CreatePixelShader(g_ShapePS, sizeof(g_ShapePS), nullptr, &PixelShader)) &&
                   SUCCEEDED(Device->CreateBlendState(&blend, &Blend)) &&
                   SUCCEEDED(Device->CreateRasterizerState(&rasterizer, &Rasterizer)) &&
                   SUCCEEDED(Device->CreateSamplerState(&sampler, &Sampler)) &&
                   SUCCEEDED(Device->CreateBuffer(&constants, nullptr, &Constants));
        }

        // On failure AtlasSize stays 0, so the next update recreates the texture again.
        bool CreateAtlas(uint32_t size, const uint8_t* pixels)
        {
            SafeRelease(AtlasView);
            SafeRelease(Atlas);
            AtlasSize = 0;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = size;
            desc.Height = size;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA data = { pixels, size * 2, 0 };
            if (FAILED(Device->CreateTexture2D(&desc, &data, &Atlas)) ||
                FAILED(Device->CreateShaderResourceView(Atlas, nullptr, &AtlasView)))
                return false;
            AtlasSize = size;
            return true;
        }

        bool SetConstants(const ShaderConstants& constants)
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(Context->Map(Constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return false;
            MemCopy(mapped.pData, &constants, sizeof(constants));
            Context->Unmap(Constants, 0);
            return true;
        }

        void BindPipeline()
        {
            D3D11_VIEWPORT viewport = { 0, 0, float(Width), float(Height), 0, 1 };
            ID3D11ShaderResourceView* views[] = { Shapes.View, Clips.View, AtlasView, Points.View };

            Context->RSSetViewports(1, &viewport);
            Context->RSSetState(Rasterizer);
            Context->OMSetBlendState(Blend, nullptr, 0xFFFFFFFF);
            Context->IASetInputLayout(nullptr);
            Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            Context->VSSetShader(VertexShader, nullptr, 0);
            Context->VSSetShaderResources(0, 1, views);
            Context->VSSetConstantBuffers(0, 1, &Constants);
            Context->PSSetShader(PixelShader, nullptr, 0);
            Context->PSSetShaderResources(0, ARRAYSIZE(views), views);
            Context->PSSetSamplers(0, 1, &Sampler);
            Context->PSSetConstantBuffers(0, 1, &Constants);
        }

        // Uploads the frame and draws it into the bound render target.
        bool Draw(const GpuShape* shapes, uint32_t shapeCount,
                  const DrawBatch* batches, uint32_t batchCount,
                  const ClipRect* clips, uint32_t clipCount,
                  const Vec2* points, uint32_t pointCount, float dpiScale)
        {
            if (shapeCount == 0)
                return true;
            if (!Shapes.Upload(Device, Context, shapes, shapeCount, sizeof(GpuShape)) ||
                !Clips.Upload(Device, Context, clips, clipCount, sizeof(ClipRect)) ||
                (pointCount > 0 && !Points.Upload(Device, Context, points, pointCount, sizeof(Vec2))))
                return false;
            BindPipeline();

            ShaderConstants constants = {};
            constants.ViewportSize[0] = float(Width) / dpiScale;
            constants.ViewportSize[1] = float(Height) / dpiScale;
            constants.DpiScale = dpiScale;
            constants.AtlasTexelSize[0] = 1.0f / float(Max(AtlasSize, 1u));
            constants.AtlasTexelSize[1] = constants.AtlasTexelSize[0];

            // Every batch samples the glyph atlas: DrawBatch::Texture only matters once images exist (stage 3).
            for (uint32_t i = 0; i < batchCount; ++i)
            {
                constants.FirstShape = batches[i].FirstShape;
                if (!SetConstants(constants))
                    return false;
                Context->DrawInstanced(4, batches[i].ShapeCount, 0, 0);
            }
            return true;
        }
    };

    bool Renderer::Init(HWND__* window, uint32_t width, uint32_t height)
    {
        void* memory = MemAlloc(sizeof(Impl));
        if (!memory)
            return false;
        State = new (memory) Impl();
        State->Window = window;
        State->Width = Max(width, 1u);
        State->Height = Max(height, 1u);
        if (State->CreateAll())
            return true;
        Shutdown();
        return false;
    }

    bool Renderer::Init(ID3D11Device* device)
    {
        void* memory = MemAlloc(sizeof(Impl));
        if (!memory)
            return false;
        State = new (memory) Impl();
        State->Embedded = true;
        State->Device = device;
        device->AddRef();
        device->GetImmediateContext(&State->Context);
        State->Width = State->Height = 1;
        if (State->CreateAll())
            return true;
        Shutdown();
        return false;
    }

    void Renderer::Shutdown()
    {
        if (!State)
            return;
        State->ReleaseAll();
        MemFree(State);
        State = nullptr;
    }

    // Every other method is a no-op (or fails) while a lost device has not been recreated (Device == null).
    bool Renderer::RestoreDevice()
    {
        Impl& s = *State;
        if (s.Embedded) // a lost client device is the client's to recreate (with a new Ui)
            return false;
        if (s.Device && s.Device->GetDeviceRemovedReason() == S_OK)
            return false;
        // While the GPU stays unavailable (e.g. during a driver install), retry only now and then.
        if (s.RestoreDelay > 0)
        {
            --s.RestoreDelay;
            return false;
        }
        s.ReleaseAll();
        if (s.CreateAll())
            return true;
        s.ReleaseAll();
        s.RestoreDelay = RestoreInterval;
        return false;
    }

    void Renderer::Resize(uint32_t width, uint32_t height)
    {
        Impl& s = *State;
        width = Max(width, 1u);
        height = Max(height, 1u);
        if (width == s.Width && height == s.Height)
            return;
        s.Width = width; // even if ResizeBuffers fails (device lost): a restored device is created at this size
        s.Height = height;
        if (!s.Device)
            return;

        // ResizeBuffers requires every reference to the old buffers to be gone, including the bound view.
        s.Context->OMSetRenderTargets(0, nullptr, nullptr);
        SafeRelease(s.BackBuffer);
        s.Context->Flush();
        s.SwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        s.CreateBackBufferView(); // on failure Render retries and reports it
    }

    void Renderer::SetTarget(ID3D11RenderTargetView* view, uint32_t width, uint32_t height)
    {
        Impl& s = *State;
        if (view)
            view->AddRef();
        SafeRelease(s.ClientTarget);
        s.ClientTarget = view;
        s.Width = Max(width, 1u);
        s.Height = Max(height, 1u);
    }

    void Renderer::UpdateAtlas(const AtlasUpdate& update)
    {
        Impl& s = *State;
        if (!s.Device)
            return; // RestoreDevice asks for the whole atlas again
        if (update.Recreate || update.Size != s.AtlasSize)
        {
            s.CreateAtlas(update.Size, update.Pixels);
            return;
        }
        if (update.Width == 0 || update.Height == 0)
            return;

        const uint32_t pitch = update.Size * 2;
        D3D11_BOX box = { update.X, update.Y, 0, update.X + update.Width, update.Y + update.Height, 1 };
        s.Context->UpdateSubresource(s.Atlas, 0, &box, update.Pixels + update.Y * pitch + update.X * 2, pitch, 0);
    }

    bool Renderer::Render(const GpuShape* shapes, uint32_t shapeCount,
                          const DrawBatch* batches, uint32_t batchCount,
                          const ClipRect* clips, uint32_t clipCount,
                          const Vec2* points, uint32_t pointCount, float dpiScale)
    {
        Impl& s = *State;
        if (s.Embedded)
        {
            // On top of the client's target, then the client's context state is given back as it was.
            bool ok = s.ClientTarget != nullptr;
            if (ok && shapeCount > 0)
            {
                s.Saved.Save(s.Context);
                s.Context->OMSetRenderTargets(1, &s.ClientTarget, nullptr);
                s.Context->GSSetShader(nullptr, nullptr, 0);
                s.Context->HSSetShader(nullptr, nullptr, 0);
                s.Context->DSSetShader(nullptr, nullptr, 0);
                ok = s.Draw(shapes, shapeCount, batches, batchCount, clips, clipCount, points, pointCount, dpiScale);
                s.Saved.Restore(s.Context);
            }
            SafeRelease(s.ClientTarget); // see SetTarget
            return ok;
        }

        if (!s.Device || (!s.BackBuffer && !s.CreateBackBufferView()))
            return false;

        // Present unbinds the back buffer (flip model), so bind it every frame.
        const float transparent[4] = {};
        s.Context->OMSetRenderTargets(1, &s.BackBuffer, nullptr);
        s.Context->ClearRenderTargetView(s.BackBuffer, transparent);
        return s.Draw(shapes, shapeCount, batches, batchCount, clips, clipCount, points, pointCount, dpiScale) &&
               SUCCEEDED(s.SwapChain->Present(1, 0));
    }

    void* Renderer::FrameWaitable() const
    {
        return State ? State->FrameWaitable : nullptr;
    }
}
