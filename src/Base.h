// Internal foundation: memory, containers, hashing, math. No CRT, no exceptions, no RTTI.
//
// Rules for all library code (see docs/ARCHITECTURE.md, "No-CRT rules"):
//   * no CRT functions (malloc, memcpy, sinf, printf, ...) — use the helpers below;
//   * no STL containers, std::format, iostreams; only header-only STL (span, string_view, bit, type_traits);
//   * no global objects with constructors, no function-local statics with dynamic initialization;
//   * no stack arrays larger than ~2 KB (would need __chkstk);
//   * no string literals in release code (use Key/consteval hashes; FK_ASSERT is debug-only).

#pragma once

#include "FunkyUI.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <xmmintrin.h>

#if defined(_DEBUG)
    #define FK_ASSERT(condition) do { if (!(condition)) __debugbreak(); } while (0)
#else
    #define FK_ASSERT(condition) do { } while (0)
#endif

namespace Funky
{
    // ------------------------------------------------------------------------------------
    // Memory
    // ------------------------------------------------------------------------------------

    // Installs the allocator used by the whole library (called by Ui::Create).
    void SetAllocator(const Allocator& allocator);

    void* MemAlloc(size_t size);            // 16-byte aligned, never returns uninitialized garbage in debug
    void* MemRealloc(void* pointer, size_t oldSize, size_t newSize);
    void MemFree(void* pointer);

    inline void MemCopy(void* dst, const void* src, size_t size) { __builtin_memcpy(dst, src, size); }
    inline void MemMove(void* dst, const void* src, size_t size) { __builtin_memmove(dst, src, size); }
    inline void MemZero(void* dst, size_t size) { __builtin_memset(dst, 0, size); }
    // Not __builtin_memcmp: with a runtime size it becomes a call to the CRT's memcmp. Compares
    // 32 bytes per branch and stops at the first difference.
    inline bool MemEqual(const void* a, const void* b, size_t size)
    {
        const uint8_t* p = static_cast<const uint8_t*>(a);
        const uint8_t* q = static_cast<const uint8_t*>(b);
        size_t i = 0;
        for (; i + 32 <= size; i += 32)
        {
            uint64_t x[4], y[4];
            __builtin_memcpy(x, p + i, 32);
            __builtin_memcpy(y, q + i, 32);
            if (((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) != 0)
                return false;
        }
        for (; i + 8 <= size; i += 8)
        {
            uint64_t x, y;
            __builtin_memcpy(&x, p + i, 8);
            __builtin_memcpy(&y, q + i, 8);
            if (x != y)
                return false;
        }
        for (; i < size; ++i)
            if (p[i] != q[i])
                return false;
        return true;
    }

    template <class T>
    T* AllocZeroed(size_t count = 1)
    {
        static_assert(std::is_trivially_destructible_v<T>);
        T* p = static_cast<T*>(MemAlloc(sizeof(T) * count));
        if (p)
            MemZero(p, sizeof(T) * count);
        return p;
    }

    // Growable array for trivially copyable types. Memory is kept on Clear() and reused.
    template <class T>
    struct Array
    {
        static_assert(std::is_trivially_copyable_v<T>);

        T* Data = nullptr;
        uint32_t Count = 0;
        uint32_t Capacity = 0;

        T& operator[](uint32_t index) { FK_ASSERT(index < Count); return Data[index]; }
        const T& operator[](uint32_t index) const { FK_ASSERT(index < Count); return Data[index]; }
        T* begin() { return Data; }
        T* end() { return Data + Count; }
        const T* begin() const { return Data; }
        const T* end() const { return Data + Count; }
        bool IsEmpty() const { return Count == 0; }
        T& Back() { FK_ASSERT(Count > 0); return Data[Count - 1]; }

        bool Reserve(uint32_t capacity)
        {
            if (capacity <= Capacity)
                return true;
            uint32_t newCapacity = Capacity ? Capacity : 16;
            while (newCapacity < capacity)
                newCapacity *= 2;
            T* p = static_cast<T*>(MemRealloc(Data, sizeof(T) * Capacity, sizeof(T) * newCapacity));
            if (!p)
                return false;
            Data = p;
            Capacity = newCapacity;
            return true;
        }

        T* Push(const T& value)
        {
            if (Count == Capacity && !Reserve(Count + 1))
                return nullptr;
            Data[Count] = value;
            return &Data[Count++];
        }

        // Appends count uninitialized elements, returns a pointer to the first one.
        T* Append(uint32_t count)
        {
            if (!Reserve(Count + count))
                return nullptr;
            T* p = Data + Count;
            Count += count;
            return p;
        }

        void Pop() { FK_ASSERT(Count > 0); --Count; }
        void Clear() { Count = 0; }

        void RemoveAt(uint32_t index)
        {
            FK_ASSERT(index < Count);
            MemMove(Data + index, Data + index + 1, sizeof(T) * (Count - index - 1));
            --Count;
        }

        void Free()
        {
            MemFree(Data);
            Data = nullptr;
            Count = Capacity = 0;
        }
    };

    // Open-addressing hash map from a non-zero 64-bit key to a trivially copyable value.
    // Key 0 is reserved as "empty". Linear probing, backward-shift deletion (no tombstones).
    template <class V>
    struct HashMap
    {
        static_assert(std::is_trivially_copyable_v<V>);

        struct Slot
        {
            uint64_t Key;
            V Value;
        };

        Slot* Slots = nullptr;
        uint32_t Count = 0;
        uint32_t Capacity = 0; // power of two

        V* Find(uint64_t key)
        {
            if (!Capacity || !key)
                return nullptr;
            uint32_t mask = Capacity - 1;
            for (uint32_t i = uint32_t(Mix(key)) & mask;; i = (i + 1) & mask)
            {
                if (Slots[i].Key == key)
                    return &Slots[i].Value;
                if (Slots[i].Key == 0)
                    return nullptr;
            }
        }

        // Returns the value for key, inserting a zero-initialized one if missing.
        // isNew (optional) is set to true when inserted. Returns nullptr only on out-of-memory.
        V* FindOrAdd(uint64_t key, bool* isNew = nullptr)
        {
            FK_ASSERT(key != 0);
            if ((Count + 1) * 4 > Capacity * 3 && !Rehash(Capacity ? Capacity * 2 : 64))
                return nullptr;
            uint32_t mask = Capacity - 1;
            for (uint32_t i = uint32_t(Mix(key)) & mask;; i = (i + 1) & mask)
            {
                if (Slots[i].Key == key)
                {
                    if (isNew)
                        *isNew = false;
                    return &Slots[i].Value;
                }
                if (Slots[i].Key == 0)
                {
                    Slots[i].Key = key;
                    MemZero(&Slots[i].Value, sizeof(V));
                    ++Count;
                    if (isNew)
                        *isNew = true;
                    return &Slots[i].Value;
                }
            }
        }

        void Remove(uint64_t key)
        {
            if (!Capacity || !key)
                return;
            uint32_t mask = Capacity - 1;
            uint32_t i = uint32_t(Mix(key)) & mask;
            while (Slots[i].Key != key)
            {
                if (Slots[i].Key == 0)
                    return;
                i = (i + 1) & mask;
            }
            RemoveSlot(i);
        }

        // Calls predicate(key, value) for every entry and removes those for which it returns true.
        template <class F>
        void RemoveIf(F&& predicate)
        {
            for (uint32_t i = 0; i < Capacity;)
            {
                if (Slots[i].Key != 0 && predicate(Slots[i].Key, Slots[i].Value))
                    RemoveSlot(i); // an entry may have shifted into slot i: re-check it
                else
                    ++i;
            }
        }

        template <class F>
        void ForEach(F&& visit)
        {
            for (uint32_t i = 0; i < Capacity; ++i)
                if (Slots[i].Key != 0)
                    visit(Slots[i].Key, Slots[i].Value);
        }

        void Clear()
        {
            if (Slots)
                MemZero(Slots, sizeof(Slot) * Capacity);
            Count = 0;
        }

        // Far larger than its use (after a burst): walking it (RemoveIf, ForEach) would cost every later frame the peak.
        bool IsSparse() const { return Capacity > 1024 && Count * 8 < Capacity; }

        // A sparse map is rebuilt smaller. Invalidates value pointers.
        void ShrinkIfSparse()
        {
            if (!IsSparse())
                return;
            uint32_t capacity = 64;
            while (Count * 4 > capacity) // at most half full afterwards
                capacity *= 2;
            Rehash(capacity);
        }

        void Free()
        {
            MemFree(Slots);
            Slots = nullptr;
            Count = Capacity = 0;
        }

    private:
        static uint64_t Mix(uint64_t k)
        {
            k ^= k >> 33;
            k *= 0xff51afd7ed558ccdull;
            k ^= k >> 33;
            return k;
        }

        bool Rehash(uint32_t newCapacity)
        {
            Slot* newSlots = AllocZeroed<Slot>(newCapacity);
            if (!newSlots)
                return false;
            Slot* oldSlots = Slots;
            uint32_t oldCapacity = Capacity;
            Slots = newSlots;
            Capacity = newCapacity;
            Count = 0;
            for (uint32_t i = 0; i < oldCapacity; ++i)
            {
                if (oldSlots[i].Key)
                {
                    V* v = FindOrAdd(oldSlots[i].Key);
                    *v = oldSlots[i].Value;
                }
            }
            MemFree(oldSlots);
            return true;
        }

        void RemoveSlot(uint32_t i)
        {
            uint32_t mask = Capacity - 1;
            for (uint32_t j = (i + 1) & mask; Slots[j].Key != 0; j = (j + 1) & mask)
            {
                uint32_t home = uint32_t(Mix(Slots[j].Key)) & mask;
                // Move j into the hole at i if its home position is not in (i, j].
                bool between = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
                if (!between)
                {
                    Slots[i] = Slots[j];
                    i = j;
                }
            }
            Slots[i].Key = 0;
            --Count;
        }
    };

    // Linear allocator for per-frame temporary data. Reset() at the start of every frame.
    struct Arena
    {
        struct Block
        {
            Block* Next;
            size_t Size;
            size_t Used;
        };

        Block* First = nullptr;
        Block* Current = nullptr;

        void* Alloc(size_t size, size_t align = 16);

        template <class T>
        T* AllocArray(size_t count)
        {
            static_assert(std::is_trivially_destructible_v<T>);
            return static_cast<T*>(Alloc(sizeof(T) * count, alignof(T) < 16 ? 16 : alignof(T)));
        }

        // Shrinks the most recent allocation to size bytes (0 gives it back).
        void Trim(void* last, size_t size)
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(Current + 1);
            uintptr_t start = reinterpret_cast<uintptr_t>(last);
            FK_ASSERT(Current && start >= base && start + size <= base + Current->Used);
            Current->Used = start + size - base;
        }

        void Reset();  // keeps the memory
        void Free();
    };

    // ------------------------------------------------------------------------------------
    // Hashing
    // ------------------------------------------------------------------------------------

    constexpr uint64_t HashCombine(uint64_t a, uint64_t b)
    {
        uint64_t h = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
        h ^= h >> 31;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 29;
        return h ? h : 1; // 0 is reserved as "empty" in HashMap
    }

    // Fast hash of an arbitrary byte range (used for text caching).
    uint64_t HashMemory(const void* data, size_t size, uint64_t seed = 0);

    // ------------------------------------------------------------------------------------
    // Math (no CRT: implemented with builtins or approximations)
    // ------------------------------------------------------------------------------------

    inline constexpr float Pi = 3.14159265358979323846f;

    template <class T> constexpr T Min(T a, T b) { return a < b ? a : b; }
    template <class T> constexpr T Max(T a, T b) { return a > b ? a : b; }
    template <class T> constexpr T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
    constexpr float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }
    constexpr float Abs(float v) { return v < 0 ? -v : v; }

    // sqrtss under any /fp model and any clang version (__builtin_sqrtf keeps an errno path that calls the CRT's sqrtf).
    inline float Sqrt(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }

    // Valid for |v| < 2^31 (plenty for UI). Not __builtin_floorf: that can become a CRT call.
    inline float Floor(float v) { float t = float(int32_t(v)); return t > v ? t - 1.0f : t; }
    inline float Ceil(float v) { float t = float(int32_t(v)); return t < v ? t + 1.0f : t; }
    inline float Round(float v) { return Floor(v + 0.5f); }

    // Approximations, max error ~1e-4 (enough for UI geometry and colors).
    float Sin(float radians);
    float Cos(float radians);
    float Atan2(float y, float x);
    float Exp(float x);
    float Pow(float base, float exponent); // base > 0

    float SrgbToLinear(float c);  // c in 0..1
    float LinearToSrgb(float c);  // c in 0..1

    struct LinearColor
    {
        float R, G, B, A;
    };

    LinearColor ToLinear(Color c);
    Color FromLinear(LinearColor c);

    // Premultiplied linear RGBA (4 floats): the space colors blend in (gradients, Transition, Lerp).
    void ToPremultipliedLinear(Color c, float* out);
    Color FromPremultipliedLinear(const float* v); // transparent when alpha <= 0

    // ------------------------------------------------------------------------------------
    // Text encoding
    // ------------------------------------------------------------------------------------

    // Converts UTF-8 to UTF-16 into a buffer allocated from the arena (null-terminated).
    // Invalid sequences become U+FFFD. Returns the number of UTF-16 units (without the terminator).
    uint32_t Utf8ToUtf16(std::string_view utf8, Arena& arena, wchar_t** out);
}
