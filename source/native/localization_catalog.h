#pragma once
// Layout of the menu translation catalogs. generate_locale.py writes
// locale_catalogs.generated.inc from locale/*.po in this shape; the runtime and
// the localization tests read it. English is the source text, never a catalog.
#include <cstddef>
#include <cstdint>

namespace l10n
{
struct Entry
{
    uint32_t hash;           // FNV-1a of the English UTF-8 bytes; entries are sorted by it.
    const char* english;
    const char* translated;  // UTF-8.
};

// A Windows font file under %WINDIR%\Fonts and the face index inside a .ttc.
struct FontFace
{
    const wchar_t* file;
    uint32_t face;
};

struct Catalog
{
    const char* code;          // BCP 47, e.g. "zh-CN"; also the RTX40MFG-UI.ini value.
    const char* nativeName;    // UTF-8, shown in the language list.
    const char* englishName;   // Shown instead when no loaded font can draw the native name.
    const char* const* matches; // Windows UI language prefixes Auto selects this catalog for.
    size_t matchCount;
    const FontFace* fonts;     // First available face is merged over the Latin UI font.
    size_t fontCount;
    const Entry* entries;
    size_t entryCount;
    const char16_t* glyphs;    // Every non-ASCII code point the catalog draws, sorted.
    size_t glyphCount;
};

constexpr uint32_t Fnv1a(const char* text) noexcept
{
    uint32_t hash = 2166136261u;
    for (; *text; ++text)
        hash = (hash ^ static_cast<unsigned char>(*text)) * 16777619u;
    return hash;
}
}
