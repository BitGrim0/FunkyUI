// TextSystem — DirectWrite shaping, glyph rasterization into an R8G8 atlas, layout cache.

#include "Internal.h"

#include <windows.h>
#include <dwrite_2.h>

#include <new>

#pragma comment(lib, "dwrite.lib")

namespace Funky
{
    namespace
    {
        constexpr uint32_t InitialAtlasSize = 1024;
        constexpr uint32_t MaxAtlasSize = 4096;
        constexpr uint32_t Padding = 1;             // empty texels between glyphs (bilinear sampling)
        constexpr uint32_t MaxUnusedFrames = 120;
        constexpr float MaxExtent = 1.0e6f;         // layout box: text never wraps or clips

        constexpr TextLayout EmptyLayout = {};

        struct FontFamily
        {
            uint64_t Hash;          // of the UTF-8 name
            wchar_t* Name;
        };

        struct GlyphEntry
        {
            uint16_t X, Y;          // atlas texels
            uint16_t Width, Height; // 0 = nothing to draw (e.g. space)
            int32_t Left, Top;      // bearing from the pen position, physical pixels
        };

        struct CachedLayout
        {
            TextLayout* Layout;     // header followed by its glyph quads, one allocation
            uint32_t LastUsed;
        };

        struct CachedFormat
        {
            IDWriteTextFormat* Format;
            uint32_t LastUsed;      // frame it last built a layout (animated font sizes make many)
        };

        struct TextState
        {
            IDWriteFactory2* Factory = nullptr;
            Array<FontFamily> Families;                 // [0] = default
            HashMap<CachedFormat> Formats;
            HashMap<GlyphEntry> Glyphs;
            HashMap<IDWriteFontFace*> Faces;            // every face used in a glyph key, AddRef'd
            HashMap<CachedLayout> Layouts;
            Array<GlyphQuad> Quads;                     // glyphs of the layout being built
            Array<uint8_t> GlyphPixels;                 // rasterization buffer

            uint8_t* Atlas = nullptr;
            uint32_t AtlasSize = 0;
            uint32_t ShelfX = Padding, ShelfY = Padding, ShelfHeight = 0;
            bool ResetPending = false;                  // the atlas is full: reset it at EndFrame
            bool Recreate = false;
            uint32_t DirtyMinX = UINT32_MAX, DirtyMinY = UINT32_MAX, DirtyMaxX = 0, DirtyMaxY = 0;

            int32_t FindFamily(uint64_t hash) const
            {
                for (uint32_t i = 0; i < Families.Count; ++i)
                    if (Families.Data[i].Hash == hash)
                        return int32_t(i);
                return -1;
            }

            int32_t AddFamily(uint64_t hash, const wchar_t* name, uint32_t length)
            {
                if (Families.Count > 0xFFFF)
                    return -1;
                size_t bytes = (length + 1) * sizeof(wchar_t);
                wchar_t* copy = static_cast<wchar_t*>(MemAlloc(bytes));
                if (!copy)
                    return -1;
                MemCopy(copy, name, bytes);
                if (!Families.Push({ hash, copy }))
                {
                    MemFree(copy);
                    return -1;
                }
                return int32_t(Families.Count - 1);
            }

            IDWriteTextFormat* GetFormat(uint64_t key, uint32_t font, const TextStyle& style, uint32_t frame)
            {
                if (CachedFormat* cached = Formats.Find(key))
                {
                    cached->LastUsed = frame;
                    return cached->Format;
                }
                IDWriteTextFormat* format = nullptr;
                if (FAILED(Factory->CreateTextFormat(Families[font].Name, nullptr, DWRITE_FONT_WEIGHT(style.FontWeight),
                                                     style.Italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                                     DWRITE_FONT_STRETCH_NORMAL, style.FontSize, L"", &format)))
                    return nullptr;
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                CachedFormat* slot = Formats.FindOrAdd(key);
                if (!slot)
                {
                    format->Release();
                    return nullptr;
                }
                *slot = { format, frame };
                return format;
            }

            bool CreateAtlas(uint32_t size)
            {
                Atlas = AllocZeroed<uint8_t>(size_t(size) * size * 2);
                AtlasSize = size;
                Recreate = true;
                return Atlas != nullptr;
            }

            // Keeps glyph texel coordinates (old texels go to the top-left of the new atlas), so glyph
            // quads emitted earlier in the frame stay valid.
            bool GrowAtlas()
            {
                uint32_t oldSize = AtlasSize;
                uint8_t* old = Atlas;
                if (!CreateAtlas(oldSize * 2))
                {
                    Atlas = old;
                    AtlasSize = oldSize;
                    return false;
                }
                for (uint32_t y = 0; y < oldSize; ++y)
                    MemCopy(Atlas + size_t(y) * AtlasSize * 2, old + size_t(y) * oldSize * 2, size_t(oldSize) * 2);
                MemFree(old);
                return true;
            }

            void ReleaseFaces()
            {
                Faces.ForEach([](uint64_t, IDWriteFontFace*& face) { face->Release(); });
                Faces.Clear();
            }

            void FreeLayouts()
            {
                Layouts.ForEach([](uint64_t, CachedLayout& entry) { MemFree(entry.Layout); });
                Layouts.Clear();
            }

            // Only between frames: glyph quads already emitted would sample someone else's texels.
            void ResetAtlas()
            {
                ReleaseFaces();
                Glyphs.Clear();
                FreeLayouts();
                MemZero(Atlas, size_t(AtlasSize) * AtlasSize * 2);
                ShelfX = ShelfY = Padding;
                ShelfHeight = 0;
                ResetPending = false;
                Recreate = true;
            }

            bool Pack(uint32_t width, uint32_t height, uint32_t& x, uint32_t& y)
            {
                if (ShelfX + width + Padding > AtlasSize)
                {
                    ShelfY += ShelfHeight;
                    ShelfX = Padding;
                    ShelfHeight = 0;
                }
                if (ShelfX + width + Padding > AtlasSize || ShelfY + height + Padding > AtlasSize)
                    return false;
                x = ShelfX;
                y = ShelfY;
                ShelfX += width + Padding;
                ShelfHeight = Max(ShelfHeight, height + Padding);
                return true;
            }

            void MarkDirty(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
            {
                DirtyMinX = Min(DirtyMinX, x);
                DirtyMinY = Min(DirtyMinY, y);
                DirtyMaxX = Max(DirtyMaxX, x + width);
                DirtyMaxY = Max(DirtyMaxY, y + height);
            }

            // Returns false only when out of memory. Glyphs larger than the whole atlas stay empty, and so
            // do new glyphs once the atlas is full, until it is reset at EndFrame.
            bool AddToAtlas(IDWriteGlyphRunAnalysis* analysis, const RECT& bounds, GlyphEntry& glyph)
            {
                uint32_t width = uint32_t(bounds.right - bounds.left);
                uint32_t height = uint32_t(bounds.bottom - bounds.top);
                if (width + 2 * Padding > MaxAtlasSize || height + 2 * Padding > MaxAtlasSize)
                    return true;
                uint32_t size = width * height;
                if (!GlyphPixels.Reserve(size))
                    return false;
                if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, GlyphPixels.Data, size)))
                    return true;

                uint32_t x = 0, y = 0;
                while (!Pack(width, height, x, y))
                {
                    if (AtlasSize == MaxAtlasSize)
                    {
                        ResetPending = true;
                        return true;
                    }
                    if (!GrowAtlas())
                        return false;
                }
                for (uint32_t row = 0; row < height; ++row)
                {
                    const uint8_t* src = GlyphPixels.Data + size_t(row) * width;
                    uint8_t* dst = Atlas + (size_t(y + row) * AtlasSize + x) * 2;
                    for (uint32_t column = 0; column < width; ++column)
                    {
                        dst[column * 2] = src[column];
                        dst[column * 2 + 1] = 0; // distance field: stage 2
                    }
                }
                MarkDirty(x, y, width, height);
                glyph = { uint16_t(x), uint16_t(y), uint16_t(width), uint16_t(height), bounds.left, bounds.top };
                return true;
            }

            // Rasterizes a glyph into the atlas if needed. May grow the atlas.
            bool GetGlyph(IDWriteFontFace* face, uint16_t index, float emSize, bool sideways, GlyphEntry& glyph)
            {
                uint64_t key = HashCombine(HashCombine(HashCombine(uint64_t(uintptr_t(face)), index),
                                                       std::bit_cast<uint32_t>(emSize)), sideways);
                if (const GlyphEntry* cached = Glyphs.Find(key))
                {
                    glyph = *cached;
                    return true;
                }

                float advance = 0;
                DWRITE_GLYPH_RUN run = {};
                run.fontFace = face;
                run.fontEmSize = emSize;
                run.glyphCount = 1;
                run.glyphIndices = &index;
                run.glyphAdvances = &advance;
                run.isSideways = sideways;

                // DirectWrite failures leave the glyph empty; only out-of-memory fails the layout.
                glyph = {};
                bool ok = true;
                IDWriteGlyphRunAnalysis* analysis = nullptr;
                if (SUCCEEDED(Factory->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                              DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DEFAULT,
                                                              DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &analysis)))
                {
                    RECT bounds = {};
                    if (SUCCEEDED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds)) &&
                        bounds.right > bounds.left && bounds.bottom > bounds.top)
                        ok = AddToAtlas(analysis, bounds, glyph);
                    analysis->Release();
                }
                if (!ok)
                    return false;

                bool isNewFace = false;
                IDWriteFontFace** faceSlot = Faces.FindOrAdd(uint64_t(uintptr_t(face)), &isNewFace);
                if (!faceSlot)
                    return false;
                if (isNewFace)
                {
                    face->AddRef();
                    *faceSlot = face;
                }
                GlyphEntry* entry = Glyphs.FindOrAdd(key);
                if (!entry)
                    return false;
                *entry = glyph;
                return true;
            }

            TextLayout* BuildLayout(std::string_view text, IDWriteTextFormat* format, float dpiScale, Arena& scratch);
        };

        // Collects the glyphs of an IDWriteTextLayout. Lives on the stack for one Draw call.
        class GlyphCollector final : public IDWriteTextRenderer
        {
        public:
            GlyphCollector(TextState& state, float dpiScale) : State(state), Dpi(dpiScale) {}

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
            {
                if (InlineIsEqualGUID(riid, __uuidof(IDWriteTextRenderer)) || InlineIsEqualGUID(riid, __uuidof(IDWritePixelSnapping)) ||
                    InlineIsEqualGUID(riid, __uuidof(IUnknown)))
                {
                    *object = static_cast<IDWriteTextRenderer*>(this);
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
            ULONG STDMETHODCALLTYPE Release() override { return 1; }

            HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* isDisabled) override
            {
                *isDisabled = FALSE;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
            {
                *transform = { 1, 0, 0, 1, 0, 0 };
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override
            {
                *pixelsPerDip = Dpi;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baselineOriginX, FLOAT baselineOriginY, DWRITE_MEASURING_MODE,
                                                   const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override
            {
                // Right-to-left runs start at the right edge and advance to the left.
                float direction = (run->bidiLevel & 1) ? -1.0f : 1.0f;
                float emSize = run->fontEmSize * Dpi;
                float penX = baselineOriginX;
                for (uint32_t i = 0; i < run->glyphCount; ++i)
                {
                    float advance = run->glyphAdvances ? run->glyphAdvances[i] : 0;
                    float x = direction < 0 ? penX - advance : penX;
                    float y = baselineOriginY;
                    if (run->glyphOffsets)
                    {
                        x += direction * run->glyphOffsets[i].advanceOffset;
                        y -= run->glyphOffsets[i].ascenderOffset;
                    }
                    penX += direction * advance;

                    GlyphEntry glyph;
                    if (!State.GetGlyph(run->fontFace, run->glyphIndices[i], emSize, run->isSideways != FALSE, glyph))
                        return E_OUTOFMEMORY;
                    if (!glyph.Width)
                        continue;

                    float left = Round(x * Dpi) + float(glyph.Left);
                    float top = Round(y * Dpi) + float(glyph.Top);
                    GlyphQuad quad = { left / Dpi, top / Dpi, float(glyph.Width) / Dpi, float(glyph.Height) / Dpi,
                                       float(glyph.X), float(glyph.Y), float(glyph.X + glyph.Width), float(glyph.Y + glyph.Height) };
                    if (!State.Quads.Push(quad))
                        return E_OUTOFMEMORY;
                }
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

        private:
            TextState& State;
            float Dpi;
        };

        TextLayout* TextState::BuildLayout(std::string_view text, IDWriteTextFormat* format, float dpiScale, Arena& scratch)
        {
            wchar_t* wide = nullptr;
            uint32_t length = Utf8ToUtf16(text, scratch, &wide);
            IDWriteTextLayout* dwLayout = nullptr;
            if (!wide || FAILED(Factory->CreateTextLayout(wide, length, format, MaxExtent, MaxExtent, &dwLayout)))
                return nullptr;

            TextLayout* layout = nullptr;
            DWRITE_TEXT_METRICS metrics = {};
            DWRITE_LINE_METRICS* lines = nullptr;
            uint32_t lineCount = 0;
            if (SUCCEEDED(dwLayout->GetMetrics(&metrics)) && metrics.lineCount &&
                (lines = scratch.AllocArray<DWRITE_LINE_METRICS>(metrics.lineCount)) != nullptr &&
                SUCCEEDED(dwLayout->GetLineMetrics(lines, metrics.lineCount, &lineCount)))
            {
                GlyphCollector collector(*this, dpiScale);
                Quads.Clear();
                if (SUCCEEDED(dwLayout->Draw(nullptr, &collector, 0, 0)))
                    layout = static_cast<TextLayout*>(MemAlloc(sizeof(TextLayout) + sizeof(GlyphQuad) * Quads.Count));
                if (layout)
                {
                    layout->Glyphs = reinterpret_cast<GlyphQuad*>(layout + 1);
                    layout->GlyphCount = Quads.Count;
                    layout->Size = { metrics.widthIncludingTrailingWhitespace, metrics.height };
                    layout->Baseline = lines[0].baseline;
                    MemCopy(layout->Glyphs, Quads.Data, sizeof(GlyphQuad) * Quads.Count);
                }
            }
            dwLayout->Release();
            return layout;
        }
    }

    struct TextSystem::Impl : TextState
    {
    };

    bool TextSystem::Init(std::string_view defaultFamily, Arena& scratch)
    {
        State = static_cast<Impl*>(MemAlloc(sizeof(Impl)));
        if (!State)
            return false;
        Impl& s = *new (State) Impl{};

        wchar_t* name = nullptr;
        uint32_t length = Utf8ToUtf16(defaultFamily, scratch, &name);
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(&s.Factory))) ||
            !name || s.AddFamily(HashMemory(defaultFamily.data(), defaultFamily.size()), name, length) != 0 ||
            !s.CreateAtlas(InitialAtlasSize))
        {
            Shutdown();
            return false;
        }
        return true;
    }

    void TextSystem::Shutdown()
    {
        if (!State)
            return;
        Impl& s = *State;
        s.ReleaseFaces();
        s.Faces.Free();
        s.Glyphs.Free();
        s.FreeLayouts();
        s.Layouts.Free();
        s.Formats.ForEach([](uint64_t, CachedFormat& entry) { entry.Format->Release(); });
        s.Formats.Free();
        for (FontFamily& family : s.Families)
            MemFree(family.Name);
        s.Families.Free();
        s.Quads.Free();
        s.GlyphPixels.Free();
        MemFree(s.Atlas);
        if (s.Factory)
            s.Factory->Release();
        MemFree(State);
        State = nullptr;
    }

    Font TextSystem::LoadFont(std::string_view family, Arena& scratch)
    {
        if (!State)
            return {};
        Impl& s = *State;
        uint64_t hash = HashMemory(family.data(), family.size());
        int32_t index = s.FindFamily(hash);
        if (index >= 0)
            return { uint16_t(index) };

        wchar_t* name = nullptr;
        uint32_t length = Utf8ToUtf16(family, scratch, &name);
        if (!name)
            return {};

        // Unknown families would silently fall back to the system font: report them as failures.
        IDWriteFontCollection* collection = nullptr;
        UINT32 collectionIndex = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(s.Factory->GetSystemFontCollection(&collection, FALSE)))
        {
            collection->FindFamilyName(name, &collectionIndex, &exists);
            collection->Release();
        }
        if (!exists)
            return {};
        index = s.AddFamily(hash, name, length);
        return { uint16_t(Max(index, 0)) };
    }

    const TextLayout* TextSystem::Layout(std::string_view text, const TextStyle& style, float dpiScale, uint32_t frame, Arena& scratch)
    {
        if (!State || !(style.FontSize > 0) || !(dpiScale > 0))
            return &EmptyLayout;
        Impl& s = *State;

        uint32_t font = style.Font.Index < s.Families.Count ? style.Font.Index : 0;
        uint64_t formatKey = HashCombine(HashCombine(HashCombine(font, std::bit_cast<uint32_t>(style.FontSize)),
                                                     uint32_t(style.FontWeight)), style.Italic);
        uint64_t key = HashCombine(HashCombine(formatKey, std::bit_cast<uint32_t>(dpiScale)), HashMemory(text.data(), text.size()));
        if (CachedLayout* cached = s.Layouts.Find(key))
        {
            cached->LastUsed = frame;
            return cached->Layout;
        }

        IDWriteTextFormat* format = s.GetFormat(formatKey, font, style, frame);
        TextLayout* layout = format ? s.BuildLayout(text, format, dpiScale, scratch) : nullptr;
        if (!layout)
            return &EmptyLayout;
        CachedLayout* entry = s.Layouts.FindOrAdd(key);
        if (!entry)
        {
            MemFree(layout);
            return &EmptyLayout;
        }
        *entry = { layout, frame };
        return layout;
    }

    bool TextSystem::TakeAtlasUpdate(AtlasUpdate& update)
    {
        if (!State)
            return false;
        Impl& s = *State;
        if (!s.Recreate && (s.DirtyMaxX <= s.DirtyMinX || s.DirtyMaxY <= s.DirtyMinY))
            return false;

        update.Pixels = s.Atlas;
        update.Size = s.AtlasSize;
        update.Recreate = s.Recreate;
        if (s.Recreate)
        {
            update.X = update.Y = 0;
            update.Width = update.Height = s.AtlasSize;
        }
        else
        {
            update.X = s.DirtyMinX;
            update.Y = s.DirtyMinY;
            update.Width = s.DirtyMaxX - s.DirtyMinX;
            update.Height = s.DirtyMaxY - s.DirtyMinY;
        }
        s.Recreate = false;
        s.DirtyMinX = s.DirtyMinY = UINT32_MAX;
        s.DirtyMaxX = s.DirtyMaxY = 0;
        return true;
    }

    void TextSystem::InvalidateAtlas()
    {
        if (State)
            State->Recreate = true;
    }

    void TextSystem::EndFrame(uint32_t frame)
    {
        if (!State)
            return;
        Impl& s = *State;
        if (s.ResetPending)
            s.ResetAtlas();
        s.Layouts.RemoveIf([frame](uint64_t, CachedLayout& entry)
        {
            if (frame - entry.LastUsed <= MaxUnusedFrames)
                return false;
            MemFree(entry.Layout);
            return true;
        });
        s.Formats.RemoveIf([frame](uint64_t, CachedFormat& entry)
        {
            if (frame - entry.LastUsed <= MaxUnusedFrames)
                return false;
            entry.Format->Release();
            return true;
        });
    }
}
