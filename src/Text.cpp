// TextSystem — text layout (DirectWrite) and glyph rasterization into an R8G8 atlas.
//
// Two layout paths produce the same TextLayout:
//   * Simple text — Latin, Greek, Cyrillic and punctuation that the style's font has glyphs for — is one glyph per
//     code point, placed by its advance and the kerning of each pair (shaped once per font and pair). It is laid out
//     from per-font tables on every call: no shaping, no cache, no hashing of the string, so text that changes every
//     frame costs as much as static text.
//   * Everything else is shaped with IDWriteTextAnalyzer (scripts, bidi, font fallback) and cached while it is on
//     screen: a layout not used for a whole frame is freed.
// Both reproduce DirectWrite's layout (natural advances, default line spacing, pens snapped to whole pixels), so a
// string looks the same whichever path it takes.

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
        constexpr float TabStopEms = 4;             // DirectWrite's default: a tab stop every 4 × font size
        constexpr wchar_t NoLocale[1] = {};         // empty locale name: glyphs chosen by script only

        constexpr TextLayout EmptyLayout = {};

        // Code points that DirectWrite draws as one glyph each, placed only by advances and pair kerning.
        // Combining marks, the soft hyphen, special spaces and format characters need shaping.
        struct CodeRange
        {
            uint32_t First, Last;
        };

        constexpr CodeRange SimpleRanges[] = {
            { 0x0020, 0x007E }, // Basic Latin
            { 0x00A0, 0x00AC }, // Latin-1 Supplement (U+00AD soft hyphen is invisible)
            { 0x00AE, 0x024F }, // ... Latin Extended-A and -B
            { 0x0370, 0x03E1 }, // Greek (U+03E2..U+03EF are Coptic)
            { 0x03F0, 0x03FF },
            { 0x0400, 0x0482 }, // Cyrillic (U+0483..U+0489 are combining marks)
            { 0x048A, 0x04FF },
            { 0x2010, 0x2027 }, // General Punctuation: dashes, quotes, bullets, ellipsis
            { 0x2030, 0x205E }, // ... per mille, primes, guillemets
            { 0x20A0, 0x20C0 }, // Currency Symbols
        };

        constexpr uint32_t SimpleCount = []
        {
            uint32_t count = 0;
            for (CodeRange range : SimpleRanges)
                count += range.Last - range.First + 1;
            return count;
        }();

        // Index of a simple code point in the per-font tables, -1 for any other.
        int32_t SimpleIndex(uint32_t c)
        {
            uint32_t base = 0;
            for (CodeRange range : SimpleRanges)
            {
                if (c < range.First)
                    return -1;
                if (c <= range.Last)
                    return int32_t(base + c - range.First);
                base += range.Last - range.First + 1;
            }
            return -1;
        }

        uint32_t SimpleCodePoint(uint32_t index)
        {
            for (CodeRange range : SimpleRanges)
            {
                if (index <= range.Last - range.First)
                    return range.First + index;
                index -= range.Last - range.First + 1;
            }
            return 0;
        }

        // DirectWrite shapes each script run separately, so kerning never crosses a change of script. Common
        // characters (digits, punctuation, symbols) belong to the run around them.
        enum class Script : uint8_t
        {
            Common,
            Latin,
            Greek,
            Cyrillic,
        };

        struct ScriptRange
        {
            uint32_t First, Last;
            Script Value;
        };

        // The letters among the simple code points (Unicode Scripts.txt); the rest is Common.
        constexpr ScriptRange SimpleLetters[] = {
            { 0x0041, 0x005A, Script::Latin }, { 0x0061, 0x007A, Script::Latin }, { 0x00AA, 0x00AA, Script::Latin },
            { 0x00BA, 0x00BA, Script::Latin }, { 0x00C0, 0x00D6, Script::Latin }, { 0x00D8, 0x00F6, Script::Latin },
            { 0x00F8, 0x024F, Script::Latin }, { 0x0370, 0x0373, Script::Greek }, { 0x0375, 0x037D, Script::Greek },
            { 0x037F, 0x0384, Script::Greek }, { 0x0386, 0x0386, Script::Greek }, { 0x0388, 0x03FF, Script::Greek },
            { 0x0400, 0x04FF, Script::Cyrillic },
        };

        struct ScriptTable
        {
            Script Values[SimpleCount];
        };

        // Script by SimpleIndex.
        constexpr ScriptTable SimpleScripts = []
        {
            ScriptTable table = {};
            uint32_t index = 0;
            for (CodeRange range : SimpleRanges)
                for (uint32_t c = range.First; c <= range.Last; ++c, ++index)
                    for (ScriptRange letters : SimpleLetters)
                        if (c >= letters.First && c <= letters.Last)
                            table.Values[index] = letters.Value;
            return table;
        }();

        // DirectWrite's hard line breaks: LF, VT, FF, CR, NEL, LS, PS.
        bool IsLineBreak(wchar_t c)
        {
            return (c >= '\n' && c <= '\r') || c == 0x85 || c == 0x2028 || c == 0x2029;
        }

        struct FontFamily
        {
            uint64_t Hash;          // of the UTF-8 name
            wchar_t* Name;
            uint32_t Index;         // in the system font collection
        };

        // A glyph in the atlas, in the form a quad needs it: physical pixels from the pen, and texels as in GpuGlyph.
        struct GlyphEntry
        {
            float Left, Top;        // bearing from the pen position
            float Width, Height;    // 0 = nothing to draw (e.g. space)
            uint32_t Texel0;        // top-left: x | y << 16
            uint32_t Texel1;        // bottom-right (exclusive)
        };

        constexpr uint32_t Unresolved = 0xFFFFFFFF; // SizedFont glyph Texel0: not looked up yet (texels are below 4096)

        struct SimpleGlyph
        {
            uint16_t Index;         // glyph index; 0 = none in this font (the text needs font fallback)
            int32_t Advance;        // design units
        };

        // FontFace::Kerning values that are not kerning.
        constexpr int16_t KernUnknown = INT16_MIN;          // not shaped yet
        constexpr int16_t KernNeedsShaping = INT16_MIN + 1; // shaping does more than kern the pair

        // A family at one weight and style. Never freed before Shutdown: an app uses a handful.
        struct FontFace
        {
            IDWriteFontFace* Face;
            DWRITE_FONT_METRICS Metrics;
            SimpleGlyph Simple[SimpleCount];    // by SimpleIndex
            int16_t* Kerning[SimpleCount];      // by first SimpleIndex: row by second SimpleIndex, allocated on first use
        };

        // A font at one em size in pixels: its simple glyphs in the atlas, looked up on first use.
        struct SizedFont
        {
            FontFace* Font;
            float EmPixels;
            GlyphEntry Glyphs[SimpleCount];     // by SimpleIndex; Texel0 == Unresolved until used
        };

        // Vertical extent of a line, DIPs. DirectWrite's default line spacing puts the line gap above the ascent.
        struct LineBox
        {
            float Above;            // line top to baseline
            float Below;            // baseline to line bottom
        };

        LineBox FontLineBox(const DWRITE_FONT_METRICS& metrics, float emSize)
        {
            float scale = emSize / float(metrics.designUnitsPerEm);
            return { float(metrics.ascent + metrics.lineGap) * scale, float(metrics.descent) * scale };
        }

        float NextTabStop(float x, float fontSize)
        {
            float stop = TabStopEms * fontSize;
            return (Floor(x / stop) + 1) * stop;
        }

        // Quad of a glyph with its pen at (penX, penY) whole pixels from the layout's top-left: pens snap to
        // pixels like DirectWrite's pixel-snapped rendering, and the bitmap was rasterized at a whole-pixel origin.
        GlyphQuad PlaceGlyph(const GlyphEntry& glyph, float penX, float penY, float dipsPerPixel)
        {
            float left = penX + glyph.Left;
            float top = penY + glyph.Top;
            return { { left * dipsPerPixel, top * dipsPerPixel, (left + glyph.Width) * dipsPerPixel, (top + glyph.Height) * dipsPerPixel },
                     glyph.Texel0, glyph.Texel1 };
        }

        void AddInk(ClipRect& ink, const GlyphQuad& q)
        {
            ink = { Min(ink.MinX, q.Rect[0]), Min(ink.MinY, q.Rect[1]), Max(ink.MaxX, q.Rect[2]), Max(ink.MaxY, q.Rect[3]) };
        }

        // Where LayoutSimple writes the glyphs: layout quads, or glyph instances ready to draw (TextSystem::GlyphTarget).
        struct QuadSink
        {
            GlyphQuad* Out;

            void operator()(uint32_t i, const GlyphQuad& q) const { Out[i] = q; }
        };

        // Round() of a coordinate that is never below -0.5 (pens and baselines): one conversion.
        float SnapPixel(float v)
        {
            return float(int32_t(v + 0.5f));
        }

        // Two-generation cache (as in Zed's GPUI): entries used this frame are in Current, entries used only last
        // frame in Previous. A hit in Previous moves the entry to Current; EndFrame frees what is left in Previous.
        // Memory follows what is on screen: an entry not used for a whole frame is gone.
        template <class T>
        struct FrameCache
        {
            HashMap<T*> Current;
            HashMap<T*> Previous;   // null = moved to Current

            T* Find(uint64_t key)
            {
                if (T** entry = Current.Find(key))
                    return *entry;
                T** old = Previous.Find(key);
                if (!old)
                    return nullptr;
                T* value = *old;
                // Out of memory: it stays in Previous, still valid until EndFrame.
                if (T** entry = Current.FindOrAdd(key))
                {
                    *entry = value;
                    *old = nullptr;
                }
                return value;
            }

            // On failure the caller keeps ownership of value.
            bool Add(uint64_t key, T* value)
            {
                T** entry = Current.FindOrAdd(key);
                if (entry)
                    *entry = value;
                return entry != nullptr;
            }

            void EndFrame()
            {
                FreeValues(Previous);
                HashMap<T*> empty = Previous;
                Previous = Current;
                Current = empty;
            }

            void Clear()
            {
                FreeValues(Current);
                FreeValues(Previous);
            }

            void Free()
            {
                Clear();
                Current.Free();
                Previous.Free();
            }

            // EndFrame walks the whole map: one far larger than its use (after a burst of one-off text) is released
            // and grows again to fit, so the burst doesn't cost every later frame.
            static void FreeValues(HashMap<T*>& map)
            {
                map.ForEach([](uint64_t, T*& value) { MemFree(value); });
                if (map.IsSparse())
                    map.Free();
                else
                    map.Clear();
            }
        };

        // One line of text for IDWriteTextAnalyzer and IDWriteFontFallback, and the sink of its script and bidi
        // analysis (stored per UTF-16 unit). Lives on the stack while the line is shaped.
        class LineAnalysis final : public IDWriteTextAnalysisSource, public IDWriteTextAnalysisSink
        {
        public:
            const wchar_t* Text = nullptr;
            uint32_t Length = 0;
            DWRITE_SCRIPT_ANALYSIS* Scripts = nullptr;
            uint8_t* Levels = nullptr;  // resolved bidi levels, odd = right-to-left

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
            {
                if (InlineIsEqualGUID(riid, __uuidof(IDWriteTextAnalysisSource)) || InlineIsEqualGUID(riid, __uuidof(IUnknown)))
                    *object = static_cast<IDWriteTextAnalysisSource*>(this);
                else if (InlineIsEqualGUID(riid, __uuidof(IDWriteTextAnalysisSink)))
                    *object = static_cast<IDWriteTextAnalysisSink*>(this);
                else
                    *object = nullptr;
                return *object ? S_OK : E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
            ULONG STDMETHODCALLTYPE Release() override { return 1; }

            HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** text, UINT32* length) override
            {
                *text = position < Length ? Text + position : nullptr;
                *length = position < Length ? Length - position : 0;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** text, UINT32* length) override
            {
                *text = position > 0 && position <= Length ? Text : nullptr;
                *length = position <= Length ? position : 0;
                return S_OK;
            }

            DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
            {
                return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
            }

            HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length, const WCHAR** locale) override
            {
                *length = position < Length ? Length - position : 0;
                *locale = NoLocale;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 position, UINT32* length, IDWriteNumberSubstitution** substitution) override
            {
                *length = position < Length ? Length - position : 0;
                *substitution = nullptr;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 position, UINT32 length, const DWRITE_SCRIPT_ANALYSIS* analysis) override
            {
                for (uint32_t i = position; i < position + length && i < Length; ++i)
                    Scripts[i] = *analysis;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 position, UINT32 length, UINT8, UINT8 resolvedLevel) override
            {
                for (uint32_t i = position; i < position + length && i < Length; ++i)
                    Levels[i] = resolvedLevel;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32, UINT32, const DWRITE_LINE_BREAKPOINT*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32, IDWriteNumberSubstitution*) override { return S_OK; }
        };

        // A font used by shaped text: the style's or one chosen by font fallback (with fallback's size scale).
        struct RunFont
        {
            IDWriteFontFace* Face;  // referenced
            float EmSize;           // DIPs
            LineBox Box;
        };

        constexpr uint16_t Invisible = 0xFFFF;      // run "font" of control characters: no glyphs

        // A run of one line with one font, script and bidi level. Glyphs in logical order, in scratch memory.
        struct ShapedRun
        {
            uint32_t Start, Length; // UTF-16 units in the line
            uint16_t Font;          // index into RunFonts, or Invisible
            uint8_t Level;
            uint16_t* Glyphs;
            float* Advances;
            DWRITE_GLYPH_OFFSET* Offsets;
            uint32_t GlyphCount;
            float Width;
        };

        struct TextState
        {
            IDWriteFactory2* Factory = nullptr;
            IDWriteFontCollection* Collection = nullptr;    // system fonts
            IDWriteFontFallback* Fallback = nullptr;        // system font fallback
            IDWriteTextAnalyzer* Analyzer = nullptr;
            Array<FontFamily> Families;                     // [0] = default
            HashMap<FontFace*> Fonts;                       // by (family, weight, italic)
            FrameCache<SizedFont> Sized;                    // by (family, weight, italic, em size in pixels)
            FrameCache<TextLayout> Shaped;                  // by (style, scale, text): header and quads in one block
            Arena FrameLayouts;                             // simple layouts, reset at EndFrame

            // The last sized font looked up (until EndFrame): consecutive text mostly has one style.
            SizedFont* LastSized = nullptr;
            uint64_t LastFontKey = 0;
            uint32_t LastFamily = 0;
            uint32_t LastWeight = 0;
            bool LastItalic = false;
            float LastEmPixels = 0;
            HashMap<GlyphEntry> Glyphs;                     // by (face, glyph index, em size in pixels)
            HashMap<IDWriteFontFace*> GlyphFaces;           // every face in a glyph key, AddRef'd: its address stays unique

            // Reused by every layout
            Array<RunFont> RunFonts;                        // Shape: fonts of the text
            Array<ShapedRun> Runs;                          // Shape: runs of the current line
            Array<GlyphQuad> Quads;                         // Shape: glyphs of the text
            Array<uint8_t> GlyphPixels;                     // rasterization buffer

            uint8_t* Atlas = nullptr;
            uint32_t AtlasSize = 0;
            uint32_t ShelfX = Padding, ShelfY = Padding, ShelfHeight = 0;
            bool ResetPending = false;                      // the atlas is full: reset it at EndFrame
            bool Recreate = false;
            uint32_t DirtyMinX = UINT32_MAX, DirtyMinY = UINT32_MAX, DirtyMaxX = 0, DirtyMaxY = 0;

            // ---- Families and fonts ----------------------------------------------------------------

            int32_t FindFamily(uint64_t hash) const
            {
                for (uint32_t i = 0; i < Families.Count; ++i)
                    if (Families.Data[i].Hash == hash)
                        return int32_t(i);
                return -1;
            }

            // Index in the system font collection, -1 when the family is not installed.
            int32_t FindInCollection(const wchar_t* name) const
            {
                UINT32 index = 0;
                BOOL exists = FALSE;
                return SUCCEEDED(Collection->FindFamilyName(name, &index, &exists)) && exists ? int32_t(index) : -1;
            }

            int32_t AddFamily(uint64_t hash, const wchar_t* name, uint32_t length, uint32_t index)
            {
                if (Families.Count > 0xFFFF)
                    return -1;
                size_t bytes = (length + 1) * sizeof(wchar_t);
                wchar_t* copy = static_cast<wchar_t*>(MemAlloc(bytes));
                if (!copy)
                    return -1;
                MemCopy(copy, name, bytes);
                if (!Families.Push({ hash, copy, index }))
                {
                    MemFree(copy);
                    return -1;
                }
                return int32_t(Families.Count - 1);
            }

            // The family's font closest to the weight and style (DirectWrite simulates missing bold / italic), with
            // the glyph index and advance of every simple code point.
            FontFace* CreateFont(const FontFamily& family, const TextStyle& style, Arena& scratch)
            {
                IDWriteFontFamily* dwFamily = nullptr;
                IDWriteFont* dwFont = nullptr;
                IDWriteFontFace* face = nullptr;
                if (SUCCEEDED(Collection->GetFontFamily(family.Index, &dwFamily)) &&
                    SUCCEEDED(dwFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT(style.FontWeight), DWRITE_FONT_STRETCH_NORMAL,
                                                             style.Italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &dwFont)))
                    dwFont->CreateFontFace(&face);
                if (dwFont)
                    dwFont->Release();
                if (dwFamily)
                    dwFamily->Release();
                if (!face)
                    return nullptr;

                uint32_t* codePoints = scratch.AllocArray<uint32_t>(SimpleCount);
                uint16_t* indices = scratch.AllocArray<uint16_t>(SimpleCount);
                DWRITE_GLYPH_METRICS* metrics = scratch.AllocArray<DWRITE_GLYPH_METRICS>(SimpleCount);
                FontFace* font = AllocZeroed<FontFace>();
                if (codePoints && indices && metrics && font)
                {
                    uint32_t count = 0;
                    for (CodeRange range : SimpleRanges)
                        for (uint32_t c = range.First; c <= range.Last; ++c)
                            codePoints[count++] = c;
                    if (SUCCEEDED(face->GetGlyphIndices(codePoints, SimpleCount, indices)) &&
                        SUCCEEDED(face->GetDesignGlyphMetrics(indices, SimpleCount, metrics, FALSE)))
                    {
                        font->Face = face;
                        face->GetMetrics(&font->Metrics);
                        for (uint32_t i = 0; i < SimpleCount; ++i)
                            font->Simple[i] = { indices[i], int32_t(metrics[i].advanceWidth) };
                        return font;
                    }
                }
                MemFree(font);
                face->Release();
                return nullptr;
            }

            FontFace* GetFont(uint64_t key, const FontFamily& family, const TextStyle& style, Arena& scratch)
            {
                if (FontFace** found = Fonts.Find(key))
                    return *found;
                FontFace* font = CreateFont(family, style, scratch);
                if (!font)
                    return nullptr;
                FontFace** slot = Fonts.FindOrAdd(key);
                if (!slot)
                {
                    font->Face->Release();
                    MemFree(font);
                    return nullptr;
                }
                *slot = font;
                return font;
            }

            SizedFont* GetSized(uint64_t fontKey, const FontFamily& family, const TextStyle& style, float emPixels, Arena& scratch)
            {
                uint64_t key = HashCombine(fontKey, std::bit_cast<uint32_t>(emPixels));
                if (SizedFont* sized = Sized.Find(key))
                    return sized;
                FontFace* font = GetFont(fontKey, family, style, scratch);
                SizedFont* sized = font ? static_cast<SizedFont*>(MemAlloc(sizeof(SizedFont))) : nullptr;
                if (!sized)
                    return nullptr;
                sized->Font = font;
                sized->EmPixels = emPixels;
                for (GlyphEntry& glyph : sized->Glyphs)
                    glyph.Texel0 = Unresolved;
                if (!Sized.Add(key, sized))
                {
                    MemFree(sized);
                    return nullptr;
                }
                return sized;
            }

            // The style's font at its size in pixels; fontKey identifies the font (family, weight, style). Null on failure.
            SizedFont* Resolve(const TextStyle& style, float dpi, Arena& scratch, const FontFamily*& family, uint64_t& fontKey)
            {
                uint32_t familyIndex = style.Font.Index < Families.Count ? style.Font.Index : 0;
                uint32_t weight = uint32_t(style.FontWeight);
                float emPixels = style.FontSize * dpi;
                family = &Families.Data[familyIndex];
                if (LastSized && LastFamily == familyIndex && LastWeight == weight && LastItalic == style.Italic && LastEmPixels == emPixels)
                {
                    fontKey = LastFontKey;
                    return LastSized;
                }
                fontKey = HashCombine(HashCombine(family->Hash, weight), style.Italic);
                SizedFont* sized = GetSized(fontKey, *family, style, emPixels, scratch);
                if (sized)
                {
                    LastSized = sized;
                    LastFontKey = fontKey;
                    LastFamily = familyIndex;
                    LastWeight = weight;
                    LastItalic = style.Italic;
                    LastEmPixels = emPixels;
                }
                return sized;
            }

            // ---- Atlas -----------------------------------------------------------------------------

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

            void ReleaseGlyphFaces()
            {
                GlyphFaces.ForEach([](uint64_t, IDWriteFontFace*& face) { face->Release(); });
                GlyphFaces.Clear();
            }

            // Only between frames: glyph quads already emitted would sample someone else's texels. Everything
            // that holds atlas positions goes with it.
            void ResetAtlas()
            {
                ReleaseGlyphFaces();
                Glyphs.Clear();
                Sized.Clear();
                LastSized = nullptr;
                Shaped.Clear();
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
                glyph = { float(bounds.left), float(bounds.top), float(width), float(height), x | y << 16,
                          (x + width) | (y + height) << 16 };
                return true;
            }

            // Rasterizes a glyph into the atlas if needed. May grow the atlas. Returns false only when out of memory.
            bool GetGlyph(IDWriteFontFace* face, uint16_t index, float emPixels, GlyphEntry& glyph)
            {
                uint64_t key = HashCombine(HashCombine(uint64_t(uintptr_t(face)), index), std::bit_cast<uint32_t>(emPixels));
                if (const GlyphEntry* cached = Glyphs.Find(key))
                {
                    glyph = *cached;
                    return true;
                }

                float advance = 0;
                DWRITE_GLYPH_RUN run = {};
                run.fontFace = face;
                run.fontEmSize = emPixels;
                run.glyphCount = 1;
                run.glyphIndices = &index;
                run.glyphAdvances = &advance;

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
                IDWriteFontFace** faceSlot = GlyphFaces.FindOrAdd(uint64_t(uintptr_t(face)), &isNewFace);
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

            // ---- Simple text -----------------------------------------------------------------------

            // Kerning of two simple code points in design units, as DirectWrite shapes the pair (GPOS or kern table,
            // whichever the font has). Shaped once per font and pair, then read from the first code point's row.
            int16_t PairKerning(FontFace& font, uint16_t first, uint16_t second)
            {
                int16_t*& row = font.Kerning[first];
                if (!row)
                {
                    row = static_cast<int16_t*>(MemAlloc(sizeof(int16_t) * SimpleCount));
                    if (!row)
                        return 0;
                    for (uint32_t i = 0; i < SimpleCount; ++i)
                        row[i] = KernUnknown;
                }
                if (row[second] == KernUnknown)
                    row[second] = ShapePair(font, first, second);
                return row[second];
            }

            // KernNeedsShaping when shaping does more than kern the two nominal glyphs: a ligature, a contextual
            // form, an offset. Text with such a pair takes the shaping path.
            int16_t ShapePair(const FontFace& font, uint16_t first, uint16_t second)
            {
                wchar_t text[2] = { wchar_t(SimpleCodePoint(first)), wchar_t(SimpleCodePoint(second)) };
                DWRITE_SCRIPT_ANALYSIS scripts[2] = {};
                uint8_t levels[2] = {};
                LineAnalysis pair;
                pair.Text = text;
                pair.Length = 2;
                pair.Scripts = scripts;
                pair.Levels = levels;
                if (FAILED(Analyzer->AnalyzeScript(&pair, 0, 2, &pair)))
                    return KernNeedsShaping;
                if (scripts[0].script != scripts[1].script) // two runs: never kerned together
                    return 0;

                const SimpleGlyph& a = font.Simple[first];
                const SimpleGlyph& b = font.Simple[second];
                uint16_t clusters[2];
                DWRITE_SHAPING_TEXT_PROPERTIES textProps[2];
                uint16_t glyphs[4];
                DWRITE_SHAPING_GLYPH_PROPERTIES glyphProps[4];
                UINT32 glyphCount = 0;
                float advances[2];
                DWRITE_GLYPH_OFFSET offsets[2];
                // An em size of designUnitsPerEm DIPs gives the advances in design units.
                if (FAILED(Analyzer->GetGlyphs(text, 2, font.Face, FALSE, FALSE, &scripts[0], NoLocale, nullptr, nullptr, nullptr, 0, 4,
                                               clusters, textProps, glyphs, glyphProps, &glyphCount)) ||
                    glyphCount != 2 || glyphs[0] != a.Index || glyphs[1] != b.Index ||
                    FAILED(Analyzer->GetGlyphPlacements(text, clusters, textProps, 2, glyphs, glyphProps, 2, font.Face,
                                                        float(font.Metrics.designUnitsPerEm), FALSE, FALSE, &scripts[0], NoLocale,
                                                        nullptr, nullptr, 0, advances, offsets)))
                    return KernNeedsShaping;
                int32_t kerning = int32_t(Round(advances[0])) - a.Advance;
                bool kernOnly = int32_t(Round(advances[1])) == b.Advance && kerning > KernNeedsShaping && kerning <= INT16_MAX &&
                                offsets[0].advanceOffset == 0 && offsets[0].ascenderOffset == 0 &&
                                offsets[1].advanceOffset == 0 && offsets[1].ascenderOffset == 0;
                return kernOnly ? int16_t(kerning) : KernNeedsShaping;
            }

            // Lays out text made only of simple code points (and '\n', '\t') that the font has glyphs for, in one pass,
            // into result (Glyphs stays null). With a sink (room for a glyph per byte of text) the glyphs go there;
            // without, it only measures: nothing placed or rasterized. Returns false when the text needs shaping; out
            // of memory leaves result empty.
            template <class Sink>
            bool LayoutSimple(std::string_view text, SizedFont& sized, float fontSize, float dpi, const Sink* sink, TextLayout& result)
            {
                FontFace& font = *sized.Font;
                uint32_t size = uint32_t(text.size());
                result = {};

                // Same arithmetic as DirectWrite (natural measuring mode): each advance, kerning included, is scaled
                // to DIPs and accumulated; lines stack by the font's line box.
                LineBox box = FontLineBox(font.Metrics, fontSize);
                float advanceScale = fontSize / float(font.Metrics.designUnitsPerEm);
                float dipsPerPixel = 1 / dpi;
                float x = 0, top = 0, width = 0;
                int32_t previous = -1;  // SimpleIndex of the previous glyph, -1 at the start of a line or after a tab
                int32_t advance = 0;    // its advance, design units, added to x with the kerning of the next glyph
                Script run = Script::Common; // script of the line's current run, Common before its first letter
                float baseline = SnapPixel(box.Above * dpi); // pixels
                uint32_t count = 0;
                ClipRect ink = EmptyBounds;

                // UTF-8 decoded here: simple code points take at most three bytes. Anything else, including
                // invalid UTF-8, goes to the shaping path.
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data());
                for (uint32_t i = 0; i < size;)
                {
                    uint32_t c = bytes[i];
                    if (c < 0x80)
                        i += 1;
                    else if (c >= 0xC2 && c < 0xE0 && i + 1 < size && (bytes[i + 1] & 0xC0) == 0x80)
                    {
                        c = (c & 0x1F) << 6 | (bytes[i + 1] & 0x3F);
                        i += 2;
                    }
                    else if (c >= 0xE0 && c < 0xF0 && i + 2 < size && (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80)
                    {
                        c = (c & 0x0F) << 12 | (bytes[i + 1] & 0x3F) << 6 | (bytes[i + 2] & 0x3F);
                        i += 3;
                        if (c < 0x800) // overlong
                            return false;
                    }
                    else
                        return false;

                    if (c == '\n' || c == '\t')
                    {
                        x += float(advance) * advanceScale;
                        previous = -1;
                        advance = 0;
                        if (c == '\t')
                            x = NextTabStop(x, fontSize);
                        else
                        {
                            width = Max(width, x);
                            x = 0;
                            top += box.Above + box.Below;
                            baseline = SnapPixel((top + box.Above) * dpi);
                            run = Script::Common;
                        }
                        continue;
                    }

                    int32_t index = SimpleIndex(c);
                    if (index < 0 || !font.Simple[index].Index)
                        return false;
                    Script script = SimpleScripts.Values[index];
                    if (previous >= 0)
                    {
                        // No kerning across a change of script: DirectWrite shapes the runs apart.
                        int32_t kerning = 0;
                        if (script == Script::Common || run == Script::Common || script == run)
                        {
                            const int16_t* row = font.Kerning[previous];
                            kerning = row ? row[index] : KernUnknown;
                            if (kerning == KernUnknown)
                                kerning = PairKerning(font, uint16_t(previous), uint16_t(index));
                            if (kerning == KernNeedsShaping)
                                return false;
                        }
                        x += float(advance + kerning) * advanceScale;
                    }
                    if (script != Script::Common)
                        run = script;
                    if (sink)
                    {
                        GlyphEntry& glyph = sized.Glyphs[index];
                        if (glyph.Texel0 == Unresolved)
                        {
                            GlyphEntry resolved;
                            if (!GetGlyph(font.Face, font.Simple[index].Index, sized.EmPixels, resolved))
                            {
                                result = {};
                                return true;
                            }
                            glyph = resolved;
                        }
                        if (glyph.Width != 0)
                        {
                            GlyphQuad q = PlaceGlyph(glyph, SnapPixel(x * dpi), baseline, dipsPerPixel);
                            (*sink)(count++, q);
                            AddInk(ink, q);
                        }
                    }
                    previous = index;
                    advance = font.Simple[index].Advance;
                }
                x += float(advance) * advanceScale;
                result.GlyphCount = count;
                result.Size = { Max(width, x), top + box.Above + box.Below };
                result.Baseline = box.Above;
                result.Ink = ink;
                return true;
            }

            // ---- Shaped text -----------------------------------------------------------------------

            // Index into RunFonts of the face at this size, added (and referenced) if new. -1 when out of memory.
            int32_t AddRunFont(IDWriteFontFace* face, float emSize)
            {
                for (uint32_t i = 0; i < RunFonts.Count; ++i)
                    if (RunFonts[i].Face == face && RunFonts[i].EmSize == emSize)
                        return int32_t(i);
                if (RunFonts.Count >= Invisible)
                    return -1;
                DWRITE_FONT_METRICS metrics;
                face->GetMetrics(&metrics);
                if (!RunFonts.Push({ face, emSize, FontLineBox(metrics, emSize) }))
                    return -1;
                face->AddRef();
                return int32_t(RunFonts.Count - 1);
            }

            bool ShapeRun(const wchar_t* line, ShapedRun& run, const DWRITE_SCRIPT_ANALYSIS& script, Arena& scratch)
            {
                const RunFont& font = RunFonts[run.Font];
                const wchar_t* text = line + run.Start;
                BOOL rightToLeft = run.Level & 1;
                uint16_t* clusters = scratch.AllocArray<uint16_t>(run.Length);
                DWRITE_SHAPING_TEXT_PROPERTIES* textProps = scratch.AllocArray<DWRITE_SHAPING_TEXT_PROPERTIES>(run.Length);
                DWRITE_SHAPING_GLYPH_PROPERTIES* glyphProps = nullptr;
                if (!clusters || !textProps)
                    return false;
                for (uint32_t capacity = run.Length * 3 / 2 + 16;; capacity *= 2) // DirectWrite's recommended first guess
                {
                    run.Glyphs = scratch.AllocArray<uint16_t>(capacity);
                    glyphProps = scratch.AllocArray<DWRITE_SHAPING_GLYPH_PROPERTIES>(capacity);
                    if (!run.Glyphs || !glyphProps)
                        return false;
                    HRESULT hr = Analyzer->GetGlyphs(text, run.Length, font.Face, FALSE, rightToLeft, &script, NoLocale, nullptr,
                                                     nullptr, nullptr, 0, capacity, clusters, textProps, run.Glyphs, glyphProps,
                                                     &run.GlyphCount);
                    if (SUCCEEDED(hr))
                        break;
                    if (hr != E_NOT_SUFFICIENT_BUFFER)
                        return false;
                }
                run.Advances = scratch.AllocArray<float>(run.GlyphCount);
                run.Offsets = scratch.AllocArray<DWRITE_GLYPH_OFFSET>(run.GlyphCount);
                if (!run.Advances || !run.Offsets ||
                    FAILED(Analyzer->GetGlyphPlacements(text, clusters, textProps, run.Length, run.Glyphs, glyphProps, run.GlyphCount,
                                                        font.Face, font.EmSize, FALSE, rightToLeft, &script, NoLocale, nullptr, nullptr,
                                                        0, run.Advances, run.Offsets)))
                    return false;
                run.Width = 0;
                for (uint32_t i = 0; i < run.GlyphCount; ++i)
                    run.Width += run.Advances[i];
                return true;
            }

            // One line (no line breaks): scripts and bidi levels, fonts (fallback where the style's font has no
            // glyphs), runs shaped in logical order, then placed in visual order. box grows to the line's fonts.
            bool ShapeLine(const wchar_t* text, uint32_t length, const FontFace& primary, const FontFamily& family, const TextStyle& style,
                           float dpi, float top, LineBox& box, float& width, Arena& scratch)
            {
                width = 0;
                if (!length)
                    return true;
                LineAnalysis line;
                line.Text = text;
                line.Length = length;
                line.Scripts = scratch.AllocArray<DWRITE_SCRIPT_ANALYSIS>(length);
                line.Levels = scratch.AllocArray<uint8_t>(length);
                uint16_t* fonts = scratch.AllocArray<uint16_t>(length);
                if (!line.Scripts || !line.Levels || !fonts)
                    return false;
                MemZero(line.Scripts, sizeof(DWRITE_SCRIPT_ANALYSIS) * length);
                MemZero(line.Levels, length);
                if (FAILED(Analyzer->AnalyzeScript(&line, 0, length, &line)) || FAILED(Analyzer->AnalyzeBidi(&line, 0, length, &line)))
                    return false;

                // Bidi rule L1: tabs, and spaces before a tab or at the end of the line, take the paragraph's level.
                bool trailing = true;
                for (uint32_t i = length; i-- > 0;)
                {
                    if (text[i] == '\t')
                        trailing = true;
                    else if (text[i] != ' ')
                        trailing = false;
                    if (trailing)
                        line.Levels[i] = 0;
                }

                // Fonts: the style's family where it has the glyphs, fallback fonts elsewhere. With no font for some
                // characters the style's font draws its missing-glyph boxes.
                for (uint32_t position = 0; position < length;)
                {
                    UINT32 mapped = 0;
                    IDWriteFont* font = nullptr;
                    FLOAT scale = 1;
                    if (FAILED(Fallback->MapCharacters(&line, position, length - position, Collection, family.Name,
                                                       DWRITE_FONT_WEIGHT(style.FontWeight),
                                                       style.Italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                                       DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale)))
                        return false;
                    IDWriteFontFace* face = nullptr;
                    if (font)
                    {
                        font->CreateFontFace(&face);
                        font->Release();
                    }
                    int32_t slot = face ? AddRunFont(face, style.FontSize * scale) : AddRunFont(primary.Face, style.FontSize);
                    if (face)
                        face->Release();
                    if (slot < 0)
                        return false;
                    mapped = Clamp(mapped, 1u, length - position);
                    for (uint32_t i = position; i < position + mapped; ++i)
                        fonts[i] = uint16_t(slot);
                    position += mapped;
                }

                // Control characters draw nothing (a tab moves to the next tab stop).
                for (uint32_t i = 0; i < length; ++i)
                    if (text[i] < 0x20 || (text[i] >= 0x7F && text[i] < 0xA0) || (line.Scripts[i].shapes & DWRITE_SCRIPT_SHAPES_NO_VISUAL))
                        fonts[i] = Invisible;

                // Runs: maximal spans of one font, script and bidi level.
                Runs.Clear();
                for (uint32_t start = 0; start < length;)
                {
                    uint32_t end = start + 1;
                    while (end < length && fonts[end] == fonts[start] && line.Levels[end] == line.Levels[start] &&
                           line.Scripts[end].script == line.Scripts[start].script)
                        ++end;
                    ShapedRun run = {};
                    run.Start = start;
                    run.Length = end - start;
                    run.Font = fonts[start];
                    run.Level = line.Levels[start];
                    if (run.Font != Invisible)
                    {
                        if (!ShapeRun(text, run, line.Scripts[start], scratch))
                            return false;
                        box.Above = Max(box.Above, RunFonts[run.Font].Box.Above);
                        box.Below = Max(box.Below, RunFonts[run.Font].Box.Below);
                    }
                    if (!Runs.Push(run))
                        return false;
                    start = end;
                }

                // Visual order, bidi rule L2: from the highest level down to the lowest odd one, reverse every
                // sequence of runs at that level or higher.
                uint32_t* order = scratch.AllocArray<uint32_t>(Runs.Count);
                if (!order)
                    return false;
                uint32_t highest = 0, lowestOdd = 0xFF;
                for (uint32_t i = 0; i < Runs.Count; ++i)
                {
                    order[i] = i;
                    highest = Max(highest, uint32_t(Runs[i].Level));
                    if (Runs[i].Level & 1)
                        lowestOdd = Min(lowestOdd, uint32_t(Runs[i].Level));
                }
                for (uint32_t level = highest; level >= lowestOdd; --level)
                {
                    for (uint32_t i = 0; i < Runs.Count; ++i)
                    {
                        if (Runs[order[i]].Level < level)
                            continue;
                        uint32_t end = i + 1;
                        while (end < Runs.Count && Runs[order[end]].Level >= level)
                            ++end;
                        for (uint32_t a = i, b = end - 1; a < b; ++a, --b)
                        {
                            uint32_t swap = order[a];
                            order[a] = order[b];
                            order[b] = swap;
                        }
                        i = end; // order[end] is below the level
                    }
                }

                float baseline = Round((top + box.Above) * dpi); // pixels: DirectWrite snaps the baseline, then offsets marks
                float x = 0;
                for (uint32_t k = 0; k < Runs.Count; ++k)
                {
                    const ShapedRun& run = Runs[order[k]];
                    if (run.Font == Invisible)
                    {
                        for (uint32_t i = run.Start; i < run.Start + run.Length; ++i)
                            if (text[i] == '\t')
                                x = NextTabStop(x, style.FontSize);
                        continue;
                    }
                    // Right-to-left glyphs are in logical order too: the first one is at the right end of the run.
                    const RunFont& font = RunFonts[run.Font];
                    bool rightToLeft = run.Level & 1;
                    float pen = rightToLeft ? x + run.Width : x;
                    for (uint32_t i = 0; i < run.GlyphCount; ++i)
                    {
                        float advance = run.Advances[i];
                        const DWRITE_GLYPH_OFFSET& offset = run.Offsets[i];
                        float glyphX = rightToLeft ? pen - advance - offset.advanceOffset : pen + offset.advanceOffset;
                        pen += rightToLeft ? -advance : advance;
                        GlyphEntry glyph;
                        if (!GetGlyph(font.Face, run.Glyphs[i], font.EmSize * dpi, glyph))
                            return false;
                        if (glyph.Width != 0 && !Quads.Push(PlaceGlyph(glyph, Round(glyphX * dpi), Round(baseline - offset.ascenderOffset * dpi), 1 / dpi)))
                            return false;
                    }
                    x += run.Width;
                }
                width = x;
                return true;
            }

            // Text with explicit line breaks (IsLineBreak, CRLF as one), no wrapping. Null on failure.
            TextLayout* Shape(std::string_view utf8, const SizedFont& sized, const FontFamily& family, const TextStyle& style,
                              float dpi, Arena& scratch)
            {
                wchar_t* text = nullptr;
                uint32_t length = Utf8ToUtf16(utf8, scratch, &text);
                if (!text)
                    return nullptr;

                Quads.Clear();
                RunFonts.Clear();
                bool ok = true;
                float top = 0, width = 0, baseline = 0;
                for (uint32_t start = 0;;)
                {
                    uint32_t end = start;
                    while (end < length && !IsLineBreak(text[end]))
                        ++end;
                    LineBox box = FontLineBox(sized.Font->Metrics, style.FontSize);
                    float lineWidth = 0;
                    ok = ShapeLine(text + start, end - start, *sized.Font, family, style, dpi, top, box, lineWidth, scratch);
                    if (!ok)
                        break;
                    if (start == 0)
                        baseline = box.Above;
                    top += box.Above + box.Below;
                    width = Max(width, lineWidth);
                    if (end == length)
                        break;
                    start = end + (text[end] == '\r' && end + 1 < length && text[end + 1] == '\n' ? 2 : 1);
                }
                for (RunFont& font : RunFonts)
                    font.Face->Release();

                TextLayout* layout = ok ? static_cast<TextLayout*>(MemAlloc(sizeof(TextLayout) + sizeof(GlyphQuad) * Quads.Count)) : nullptr;
                if (layout)
                {
                    layout->Glyphs = reinterpret_cast<GlyphQuad*>(layout + 1);
                    layout->GlyphCount = Quads.Count;
                    layout->Size = { width, top };
                    layout->Baseline = baseline;
                    layout->Ink = EmptyBounds;
                    MemCopy(layout->Glyphs, Quads.Data, sizeof(GlyphQuad) * Quads.Count);
                    for (const GlyphQuad& q : Quads)
                        AddInk(layout->Ink, q);
                }
                return layout;
            }

            // Shaped text from the cache, or shaped and cached. Null on failure.
            TextLayout* LayoutShaped(std::string_view text, const SizedFont& sized, uint64_t fontKey, const FontFamily& family,
                                     const TextStyle& style, float dpi, Arena& scratch)
            {
                uint64_t key = HashCombine(HashCombine(HashCombine(fontKey, std::bit_cast<uint32_t>(style.FontSize)), std::bit_cast<uint32_t>(dpi)),
                                           HashMemory(text.data(), text.size()));
                if (TextLayout* cached = Shaped.Find(key))
                    return cached;
                TextLayout* shaped = Shape(text, sized, family, style, dpi, scratch);
                if (shaped && !Shaped.Add(key, shaped))
                {
                    MemFree(shaped);
                    return nullptr;
                }
                return shaped;
            }
        };
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

        // The default family must be installed: it is the font of every style without an explicit one.
        wchar_t* name = nullptr;
        uint32_t length = Utf8ToUtf16(defaultFamily, scratch, &name);
        int32_t index = -1;
        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(&s.Factory))) &&
            SUCCEEDED(s.Factory->GetSystemFontCollection(&s.Collection, FALSE)) &&
            SUCCEEDED(s.Factory->GetSystemFontFallback(&s.Fallback)) &&
            SUCCEEDED(s.Factory->CreateTextAnalyzer(&s.Analyzer)) && name)
            index = s.FindInCollection(name);
        if (index < 0 || s.AddFamily(HashMemory(defaultFamily.data(), defaultFamily.size()), name, length, uint32_t(index)) != 0 ||
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
        s.Shaped.Free();
        s.Sized.Free();
        s.ReleaseGlyphFaces();
        s.GlyphFaces.Free();
        s.Glyphs.Free();
        s.Fonts.ForEach([](uint64_t, FontFace*& font)
        {
            for (int16_t* row : font->Kerning)
                MemFree(row);
            font->Face->Release();
            MemFree(font);
        });
        s.Fonts.Free();
        for (FontFamily& family : s.Families)
            MemFree(family.Name);
        s.Families.Free();
        s.FrameLayouts.Free();
        s.RunFonts.Free();
        s.Runs.Free();
        s.Quads.Free();
        s.GlyphPixels.Free();
        MemFree(s.Atlas);
        if (s.Analyzer)
            s.Analyzer->Release();
        if (s.Fallback)
            s.Fallback->Release();
        if (s.Collection)
            s.Collection->Release();
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

        // Unknown families would silently fall back to other fonts: report them as failures.
        wchar_t* name = nullptr;
        uint32_t length = Utf8ToUtf16(family, scratch, &name);
        int32_t collectionIndex = name ? s.FindInCollection(name) : -1;
        if (collectionIndex < 0)
            return {};
        index = s.AddFamily(hash, name, length, uint32_t(collectionIndex));
        return { uint16_t(Max(index, 0)) };
    }

    const TextLayout* TextSystem::Layout(std::string_view text, const TextStyle& style, float dpiScale, Arena& scratch)
    {
        if (!State || !(style.FontSize > 0) || !(dpiScale > 0))
            return &EmptyLayout;
        Impl& s = *State;
        const FontFamily* family = nullptr;
        uint64_t fontKey = 0;
        SizedFont* sized = s.Resolve(style, dpiScale, scratch, family, fontKey);
        if (!sized)
            return &EmptyLayout;

        // Simple text is laid out in place, valid until EndFrame: the header and room for a glyph per byte,
        // trimmed to the glyphs placed.
        TextLayout* layout = static_cast<TextLayout*>(s.FrameLayouts.Alloc(sizeof(TextLayout) + sizeof(GlyphQuad) * text.size()));
        if (!layout)
            return &EmptyLayout;
        QuadSink quads = { reinterpret_cast<GlyphQuad*>(layout + 1) };
        if (s.LayoutSimple(text, *sized, style.FontSize, dpiScale, &quads, *layout))
        {
            layout->Glyphs = quads.Out;
            s.FrameLayouts.Trim(layout, sizeof(TextLayout) + sizeof(GlyphQuad) * layout->GlyphCount);
            return layout;
        }
        s.FrameLayouts.Trim(layout, 0);
        const TextLayout* shaped = s.LayoutShaped(text, *sized, fontKey, *family, style, dpiScale, scratch);
        return shaped ? shaped : &EmptyLayout;
    }

    Vec2 TextSystem::Measure(std::string_view text, const TextStyle& style, float dpiScale, Arena& scratch)
    {
        if (!State || !(style.FontSize > 0) || !(dpiScale > 0))
            return {};
        Impl& s = *State;
        const FontFamily* family = nullptr;
        uint64_t fontKey = 0;
        SizedFont* sized = s.Resolve(style, dpiScale, scratch, family, fontKey);
        if (!sized)
            return {};
        TextLayout measured;
        if (s.LayoutSimple(text, *sized, style.FontSize, dpiScale, static_cast<const QuadSink*>(nullptr), measured))
            return measured.Size;
        // Shaped text: its layout is cached and usually drawn right after.
        const TextLayout* shaped = s.LayoutShaped(text, *sized, fontKey, *family, style, dpiScale, scratch);
        return shaped ? shaped->Size : Vec2{};
    }

    const TextLayout* TextSystem::LayoutInto(std::string_view text, const TextStyle& style, float dpiScale, Arena& scratch,
                                             const GlyphTarget& target, uint32_t& count, ClipRect& ink)
    {
        count = 0;
        if (!State || !(style.FontSize > 0) || !(dpiScale > 0))
            return nullptr;
        Impl& s = *State;
        const FontFamily* family = nullptr;
        uint64_t fontKey = 0;
        SizedFont* sized = s.Resolve(style, dpiScale, scratch, family, fontKey);
        if (!sized)
            return nullptr;
        TextLayout result;
        if (!s.LayoutSimple(text, *sized, style.FontSize, dpiScale, &target, result))
        {
            const TextLayout* shaped = s.LayoutShaped(text, *sized, fontKey, *family, style, dpiScale, scratch);
            return shaped ? shaped : &EmptyLayout;
        }
        count = result.GlyphCount;
        ink = { target.Origin.X + result.Ink.MinX, target.Origin.Y + result.Ink.MinY, target.Origin.X + result.Ink.MaxX,
                target.Origin.Y + result.Ink.MaxY };
        return nullptr;
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

    void TextSystem::EndFrame()
    {
        if (!State)
            return;
        Impl& s = *State;
        s.LastSized = nullptr; // may be freed by Sized.EndFrame below or the next one
        if (s.ResetPending)
            s.ResetAtlas();
        s.Shaped.EndFrame();
        s.Sized.EndFrame();

        // Like FrameCache::FreeValues: after a burst of text the arena is released and grows again to fit.
        size_t used = 0, capacity = 0;
        for (Arena::Block* block = s.FrameLayouts.First; block; block = block->Next)
        {
            used += block->Used;
            capacity += block->Size;
        }
        if (capacity > (1u << 20) && used * 8 < capacity)
            s.FrameLayouts.Free();
        else
            s.FrameLayouts.Reset();
    }
}
