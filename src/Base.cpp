// Base — memory, arena, hashing, math approximations, color space, UTF-8 → UTF-16.

#include "Base.h"

#include <windows.h>

namespace Funky
{
    namespace
    {
        constinit Allocator CustomAllocator; // Allocate == nullptr: process heap

#if defined(_DEBUG)
        constexpr DWORD HeapFlags = HEAP_ZERO_MEMORY;
#else
        constexpr DWORD HeapFlags = 0;
#endif

        constexpr size_t MinArenaBlock = 64 * 1024;

        void* TryAlloc(Arena::Block* block, size_t size, size_t align)
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(block + 1);
            uintptr_t start = (base + block->Used + align - 1) & ~uintptr_t(align - 1);
            if (start + size > base + block->Size)
                return nullptr;
            block->Used = start + size - base;
            return reinterpret_cast<void*>(start);
        }

        uint64_t MixWord(uint64_t w)
        {
            w *= 0x87C37B91114253D5ull;
            w = std::rotl(w, 31);
            return w * 0x4CF5AD432745937Full;
        }

        // 2^x: 2^round(x) from the exponent bits times a polynomial on [-0.5, 0.5].
        float Exp2(float x)
        {
            x = Clamp(x, -126.0f, 126.0f);
            float i = Round(x);
            float f = x - i;
            float p = 1 + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
            return p * std::bit_cast<float>(uint32_t(int32_t(i) + 127) << 23);
        }

        // log2(x), x > 0: exponent bits plus the atanh series of the mantissa in [sqrt(1/2), sqrt(2)).
        float Log2(float x)
        {
            uint32_t bits = std::bit_cast<uint32_t>(x);
            int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127;
            float m = std::bit_cast<float>((bits & 0x7FFFFF) | 0x3F800000);
            if (m > 1.41421356f)
            {
                m *= 0.5f;
                ++exponent;
            }
            float s = (m - 1) / (m + 1);
            float s2 = s * s;
            return float(exponent) + s * (2.88539008f + s2 * (0.96179669f + s2 * (0.57707801f + s2 * 0.41219858f)));
        }
    }

    // ------------------------------------------------------------------------------------
    // Memory
    // ------------------------------------------------------------------------------------

    void SetAllocator(const Allocator& allocator)
    {
        CustomAllocator = (allocator.Allocate && allocator.Free) ? allocator : Allocator{};
    }

    void* MemAlloc(size_t size)
    {
        if (!CustomAllocator.Allocate)
            return HeapAlloc(GetProcessHeap(), HeapFlags, size);
        void* p = CustomAllocator.Allocate(size, CustomAllocator.User);
#if defined(_DEBUG)
        if (p)
            MemZero(p, size);
#endif
        return p;
    }

    void* MemRealloc(void* pointer, size_t oldSize, size_t newSize)
    {
        if (!pointer)
            return MemAlloc(newSize);
        if (!CustomAllocator.Allocate)
            return HeapReAlloc(GetProcessHeap(), HeapFlags, pointer, newSize);
        void* p = MemAlloc(newSize);
        if (p)
        {
            MemCopy(p, pointer, Min(oldSize, newSize));
            MemFree(pointer);
        }
        return p;
    }

    void MemFree(void* pointer)
    {
        if (!pointer)
            return;
        if (CustomAllocator.Free)
            CustomAllocator.Free(pointer, CustomAllocator.User);
        else
            HeapFree(GetProcessHeap(), 0, pointer);
    }

    // ------------------------------------------------------------------------------------
    // Arena
    // ------------------------------------------------------------------------------------

    void* Arena::Alloc(size_t size, size_t align)
    {
        FK_ASSERT(align && (align & (align - 1)) == 0);
        for (Block* block = Current; block; block = block->Next)
        {
            if (void* p = TryAlloc(block, size, align))
            {
                Current = block;
                return p;
            }
        }

        size_t capacity = Max(size + align, MinArenaBlock);
        Block* block = static_cast<Block*>(MemAlloc(sizeof(Block) + capacity));
        if (!block)
            return nullptr;
        block->Size = capacity;
        block->Used = 0;
        // Insert after the current block: the blocks behind it (kept by Reset) stay reachable.
        if (Current)
        {
            block->Next = Current->Next;
            Current->Next = block;
        }
        else
        {
            block->Next = nullptr;
            First = block;
        }
        Current = block;
        return TryAlloc(block, size, align);
    }

    void Arena::Reset()
    {
        for (Block* block = First; block; block = block->Next)
            block->Used = 0;
        Current = First;
    }

    void Arena::Free()
    {
        while (First)
        {
            Block* next = First->Next;
            MemFree(First);
            First = next;
        }
        Current = nullptr;
    }

    // ------------------------------------------------------------------------------------
    // Hashing (MurmurHash3-style word mixing, one 64-bit lane)
    // ------------------------------------------------------------------------------------

    uint64_t HashMemory(const void* data, size_t size, uint64_t seed)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint64_t h = seed ^ (uint64_t(size) * 0x9E3779B97F4A7C15ull);
        for (; size >= 8; p += 8, size -= 8)
        {
            uint64_t w;
            MemCopy(&w, p, 8);
            h ^= MixWord(w);
            h = std::rotl(h, 27) * 5 + 0x52DCE729;
        }
        if (size)
        {
            uint64_t w = 0;
            for (size_t i = 0; i < size; ++i)
                w |= uint64_t(p[i]) << (i * 8);
            h ^= MixWord(w);
        }
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h ? h : 1;
    }

    // ------------------------------------------------------------------------------------
    // Math
    // ------------------------------------------------------------------------------------

    float Sin(float radians)
    {
        // Reduce to [-pi, pi], then fold into [-pi/2, pi/2] with sin(pi - x) = sin(x).
        float x = radians - Round(radians * (0.5f / Pi)) * (2 * Pi);
        if (x > 0.5f * Pi)
            x = Pi - x;
        else if (x < -0.5f * Pi)
            x = -Pi - x;
        float x2 = x * x;
        return x * (1 + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
    }

    float Cos(float radians)
    {
        return Sin(radians + 0.5f * Pi);
    }

    float Atan2(float y, float x)
    {
        float ax = Abs(x);
        float ay = Abs(y);
        float hi = Max(ax, ay);
        if (hi == 0)
            return 0;
        float a = Min(ax, ay) / hi;
        float s = a * a;
        float r = a * (0.99997726f + s * (-0.33262347f + s * (0.19354346f + s * (-0.11643287f + s * (0.05265332f + s * -0.01172120f)))));
        if (ay > ax)
            r = 0.5f * Pi - r;
        if (x < 0)
            r = Pi - r;
        return y < 0 ? -r : r;
    }

    float Exp(float x)
    {
        return Exp2(x * 1.44269504f);
    }

    float Pow(float base, float exponent)
    {
        return base > 0 ? Exp2(exponent * Log2(base)) : 0;
    }

    float SrgbToLinear(float c)
    {
        return c <= 0.04045f ? c * (1 / 12.92f) : Pow((c + 0.055f) * (1 / 1.055f), 2.4f);
    }

    float LinearToSrgb(float c)
    {
        return c <= 0.0031308f ? c * 12.92f : 1.055f * Pow(c, 1 / 2.4f) - 0.055f;
    }

    LinearColor ToLinear(Color c)
    {
        constexpr float Unit = 1 / 255.0f;
        return { SrgbToLinear(float(c.R) * Unit), SrgbToLinear(float(c.G) * Unit), SrgbToLinear(float(c.B) * Unit), float(c.A) * Unit };
    }

    Color FromLinear(LinearColor c)
    {
        auto byte = [](float v) { return uint8_t(Saturate(v) * 255 + 0.5f); };
        return { byte(LinearToSrgb(Saturate(c.R))), byte(LinearToSrgb(Saturate(c.G))), byte(LinearToSrgb(Saturate(c.B))), byte(c.A) };
    }

    void ToPremultipliedLinear(Color color, float* out)
    {
        LinearColor c = ToLinear(color);
        out[0] = c.R * c.A;
        out[1] = c.G * c.A;
        out[2] = c.B * c.A;
        out[3] = c.A;
    }

    Color FromPremultipliedLinear(const float* v)
    {
        float alpha = v[3];
        if (alpha <= 0)
            return Colors::Transparent;
        return FromLinear({ v[0] / alpha, v[1] / alpha, v[2] / alpha, alpha });
    }

    // Premultiplied, like the shader's gradients: fading to or from transparent does not darken.
    Color Lerp(Color a, Color b, float t)
    {
        float x[4], y[4], mixed[4];
        ToPremultipliedLinear(a, x);
        ToPremultipliedLinear(b, y);
        for (int i = 0; i < 4; ++i)
            mixed[i] = Lerp(x[i], y[i], t);
        return FromPremultipliedLinear(mixed);
    }

    // ------------------------------------------------------------------------------------
    // Text encoding
    // ------------------------------------------------------------------------------------

    uint32_t Utf8ToUtf16(std::string_view utf8, Arena& arena, wchar_t** out)
    {
        // Every UTF-8 byte yields at most one UTF-16 unit (4-byte sequences: 2 units).
        wchar_t* buffer = arena.AllocArray<wchar_t>(utf8.size() + 1);
        *out = buffer;
        if (!buffer)
            return 0;

        const uint8_t* s = reinterpret_cast<const uint8_t*>(utf8.data());
        size_t size = utf8.size();
        uint32_t count = 0;
        for (size_t i = 0; i < size;)
        {
            uint32_t c = s[i];
            if (c < 0x80)
            {
                buffer[count++] = wchar_t(c);
                ++i;
                continue;
            }

            // Sequence length and the valid range of the second byte (rejects overlongs,
            // surrogates and code points above U+10FFFF).
            uint32_t length = 0;
            uint32_t lo = 0x80;
            uint32_t hi = 0xBF;
            if (c >= 0xC2 && c <= 0xDF)
            {
                length = 2;
                c &= 0x1F;
            }
            else if (c >= 0xE0 && c <= 0xEF)
            {
                length = 3;
                c &= 0x0F;
                lo = c == 0x0 ? 0xA0 : 0x80;
                hi = c == 0xD ? 0x9F : 0xBF;
            }
            else if (c >= 0xF0 && c <= 0xF4)
            {
                length = 4;
                c &= 0x07;
                lo = c == 0 ? 0x90 : 0x80;
                hi = c == 4 ? 0x8F : 0xBF;
            }

            // An invalid sequence becomes one U+FFFD for its longest valid prefix.
            uint32_t n = 1;
            for (; n < length && i + n < size; ++n)
            {
                uint32_t b = s[i + n];
                if (b < lo || b > hi)
                    break;
                c = (c << 6) | (b & 0x3F);
                lo = 0x80;
                hi = 0xBF;
            }
            i += n;

            if (n < length || length == 0)
                buffer[count++] = 0xFFFD;
            else if (c >= 0x10000)
            {
                c -= 0x10000;
                buffer[count++] = wchar_t(0xD800 + (c >> 10));
                buffer[count++] = wchar_t(0xDC00 + (c & 0x3FF));
            }
            else
                buffer[count++] = wchar_t(c);
        }
        buffer[count] = 0;
        return count;
    }
}
