// l10n runtime against the fixture catalogs (fixtures/zz-Hant.po, zz-ZZ.po):
// Auto resolution, explicit settings, lookup, English fallback, the per-entry
// format guard, hash collisions, labels and the shared format signature cases.
#include "localization.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace
{
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL %s\n", what); }
}

const l10n::Catalog* Find(const char* code)
{
    for (size_t i = 0; i < l10n::CatalogCount(); ++i)
        if (!strcmp(l10n::CatalogAt(i).code, code)) return &l10n::CatalogAt(i);
    return nullptr;
}

std::string Unescape(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { out += '\n'; ++i; }
        else out += text[i];
    }
    return out;
}
}

int main(int argc, char** argv)
{
    const l10n::Catalog* zed = Find("zz-ZZ");
    const l10n::Catalog* hant = Find("zz-Hant");
    Check(l10n::CatalogCount() == 2 && zed && hant, "two fixture catalogs compiled in");
    if (!zed || !hant) return 1;
    Check(zed->entryCount == 3, "untranslated and fuzzy entries are not compiled");
    Check(hant->fontCount == 1 && !wcscmp(hant->fonts[0].file, L"msjh.ttc") && hant->fonts[0].face == 0, "font list");
    Check(zed->fontCount == 0, "a catalog without X-MFG-Fonts uses the UI font");
    bool sorted = true;
    for (size_t i = 1; i < hant->glyphCount; ++i) sorted = sorted && hant->glyphs[i - 1] < hant->glyphs[i];
    Check(hant->glyphCount == 4 && sorted, "glyphs: native name and translations, sorted and unique");

    // Auto: first preferred UI language a catalog covers; English stops the search.
    Check(l10n::Resolve(l10n::kAuto, {L"zz-ZZ"}) == zed, "exact match");
    Check(l10n::Resolve(l10n::kAuto, {L"ZZ-zz"}) == zed, "case-insensitive match");
    Check(l10n::Resolve(l10n::kAuto, {L"zz-Hant-TW"}) == hant, "prefix covers a longer tag");
    Check(l10n::Resolve(l10n::kAuto, {L"zz-Hantx"}) == nullptr, "prefix matches whole subtags only");
    Check(l10n::Resolve(l10n::kAuto, {L"zz"}) == nullptr, "bare language not covered by longer prefixes");
    Check(l10n::Resolve(l10n::kAuto, {L"fr-FR", L"zz-TW"}) == hant, "unmatched languages are skipped");
    Check(l10n::Resolve(l10n::kAuto, {L"en-GB", L"zz-ZZ"}) == nullptr, "a preferred English stops the search");
    Check(l10n::Resolve(l10n::kAuto, {}) == nullptr, "no UI language is English");
    Check(l10n::Resolve("", {L"zz-ZZ"}) == zed, "an empty setting is Auto");
    Check(l10n::Resolve("en", {L"zz-ZZ"}) == nullptr, "explicit English");
    Check(l10n::Resolve("zz-hant", {L"zz-ZZ"}) == hant, "explicit code, any case");
    Check(l10n::Resolve("xx-YY", {L"zz-ZZ"}) == zed, "an unknown code falls back to Auto");

    l10n::Select("xx-YY");
    Check(l10n::Selection() == l10n::kAuto, "unknown saved setting becomes Auto");
    l10n::Select("zz-ZZ");
    Check(l10n::Selection() == "zz-ZZ" && l10n::Selected() == zed, "select a catalog");
    l10n::Select("en");
    Check(l10n::Selection() == "en" && l10n::Selected() == nullptr, "select English");

    // Lookup follows the displayed catalog only.
    const char* english = "Diagnostics";
    l10n::SetDisplayed(nullptr);
    Check(l10n::T(english) == english, "English display returns the same pointer");
    Check(!strcmp(l10n::Label(english), "Diagnostics###Diagnostics"), "English label keeps the stable ID");
    l10n::SetDisplayed(zed);
    Check(!strcmp(l10n::T(english), "Zdiagnostics"), "translation");
    Check(!strcmp(l10n::T("Preset %s is saved."), "Zpreset %s zsaved."), "format translation");
    Check(!strcmp(l10n::T("Line one\nline two"), "Zline one\nzline two"), "multi-line translation");
    const char* missing = "Untranslated";
    Check(l10n::T(missing) == missing, "untranslated entry shows English");
    const char* fuzzy = "Fuzzy";
    Check(l10n::T(fuzzy) == fuzzy, "fuzzy entry shows English");
    Check(l10n::T("") [0] == 0 && l10n::T(nullptr) == nullptr, "empty and null pass through");
    Check(!strcmp(l10n::Label(english), "Zdiagnostics###Diagnostics"), "label: translation###English");
    l10n::SetDisplayed(hant);
    Check(!strcmp(l10n::Label(english), "\xE8\xA8\xBA\xE6\x96\xB7###Diagnostics"), "labels rebuilt on a language change");

    // Hand-built catalog: a wrong placeholder never reaches printf, and two
    // entries sharing a hash are told apart by their text.
    constexpr uint32_t savedHash = l10n::Fnv1a("Saved %s");
    constexpr uint32_t betaHash = l10n::Fnv1a("Beta");
    const l10n::Entry saved{savedHash, "Saved %s", "Saved %d"};
    const l10n::Entry other{betaHash, "Other", "O1"}; // same hash as "Beta", different text
    const l10n::Entry beta{betaHash, "Beta", "B1"};
    const l10n::Entry low[] = {saved, other, beta}, high[] = {other, beta, saved};
    const l10n::Catalog guarded{"zz-XX", "x", "x", nullptr, 0, nullptr, 0,
        savedHash < betaHash ? low : high, 3, nullptr, 0};
    l10n::SetDisplayed(&guarded);
    const char* savedText = "Saved %s";
    Check(l10n::T(savedText) == savedText, "mismatched placeholders fall back to English");
    Check(!strcmp(l10n::T("Beta"), "B1"), "a shared hash is resolved by the text");
    const char* otherText = "Other";
    Check(l10n::T(otherText) == otherText, "an entry is found only under its own hash");
    l10n::SetDisplayed(nullptr);

    // Shared signature cases (also checked against generate_locale.py).
    std::ifstream cases(argc > 1 ? argv[1] : "format_cases.txt");
    Check(cases.good(), "format cases file opened");
    std::string line;
    int signatures = 0;
    while (std::getline(cases, line))
    {
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) { Check(false, "format case without a tab"); continue; }
        const std::string text = Unescape(line.substr(0, tab));
        const std::string expected = line.substr(tab + 1) == "-" ? std::string() : line.substr(tab + 1);
        const std::string actual = l10n::FormatSignature(text.c_str());
        if (actual != expected) std::printf("  signature of '%s': '%s', expected '%s'\n", text.c_str(), actual.c_str(), expected.c_str());
        Check(actual == expected, "format signature case");
        ++signatures;
    }
    Check(signatures >= 10, "format cases read");

    std::printf("%s: %d checks, %d failures\n", gFailures ? "FAIL" : "PASS", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
