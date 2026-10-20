#include "localization.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace l10n
{
namespace
{
#include "locale_catalogs.generated.inc"

std::string gSelection = kAuto;
const Catalog* gSelected = nullptr;
const Catalog* gDisplayed = nullptr;
// Per catalog: which entries passed the format check (computed once).
std::unordered_map<const Catalog*, std::vector<bool>> gUsable;
const std::vector<bool>* gDisplayedUsable = nullptr;
std::unordered_map<const char*, std::string> gLabels;

bool PrefixMatches(std::wstring_view language, std::string_view prefix) noexcept
{
    if (prefix.empty() || language.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        const wchar_t a = static_cast<wchar_t>(towlower(language[i]));
        const wchar_t b = static_cast<wchar_t>(towlower(static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    // Whole subtags only: "zh-Hans" covers "zh-Hans-CN", not "zh-Hansx".
    return language.size() == prefix.size() || language[prefix.size()] == L'-';
}

std::vector<std::wstring> PreferredUiLanguages()
{
    ULONG count = 0, size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || !size) return {};
    std::wstring buffer(size, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) return {};
    std::vector<std::wstring> languages;
    for (const wchar_t* at = buffer.c_str(); *at; at += wcslen(at) + 1) languages.emplace_back(at);
    return languages;
}

const Catalog* FindCode(std::string_view code) noexcept
{
    for (size_t index = 0; index < kCatalogCount; ++index)
        if (const auto& catalog = kCatalogs[index]; _strnicmp(catalog.code, code.data(), code.size()) == 0 && !catalog.code[code.size()]) return &catalog;
    return nullptr;
}

const std::vector<bool>& Usable(const Catalog& catalog)
{
    auto found = gUsable.find(&catalog);
    if (found != gUsable.end()) return found->second;
    std::vector<bool> usable(catalog.entryCount);
    for (size_t i = 0; i < catalog.entryCount; ++i)
    {
        const auto& entry = catalog.entries[i];
        usable[i] = entry.translated && entry.translated[0]
            && FormatSignature(entry.english) == FormatSignature(entry.translated);
    }
    return gUsable.emplace(&catalog, std::move(usable)).first->second;
}
}

std::string FormatSignature(const char* text)
{
    std::string signature;
    if (!text) return signature;
    for (const char* at = text; *at; ++at)
    {
        if (*at != '%') continue;
        ++at;
        if (*at == '%') continue;
        std::string token = "%";
        while (*at && strchr("-+ #0", *at)) ++at;
        if (*at == '*') { token += '*'; ++at; }
        else while (*at >= '0' && *at <= '9') ++at;
        if (*at == '.')
        {
            ++at;
            if (*at == '*') { token += ".*"; ++at; }
            else while (*at >= '0' && *at <= '9') ++at;
        }
        static constexpr const char* lengths[] = {"I64", "I32", "hh", "ll", "h", "l", "j", "z", "t", "L", "I"};
        for (const char* length : lengths)
            if (!strncmp(at, length, strlen(length))) { token += length; at += strlen(length); break; }
        if (!*at) { signature += token + "?;"; break; }
        // Unknown conversions are kept verbatim, so "%TEMP%" must survive as is.
        token += *at;
        signature += token + ';';
    }
    return signature;
}

const char* T(const char* english) noexcept
{
    const Catalog* catalog = gDisplayed;
    if (!catalog || !english || !english[0] || !gDisplayedUsable) return english;
    const uint32_t hash = Fnv1a(english);
    const Entry* first = catalog->entries;
    const Entry* last = first + catalog->entryCount;
    const Entry* at = std::lower_bound(first, last, hash,
        [](const Entry& entry, uint32_t value) { return entry.hash < value; });
    for (; at != last && at->hash == hash; ++at)
        if (!strcmp(at->english, english))
            return (*gDisplayedUsable)[static_cast<size_t>(at - first)] ? at->translated : english;
    return english;
}

const char* Label(const char* english)
{
    auto& label = gLabels[english];
    if (label.empty()) label = std::string(T(english)) + "###" + english;
    return label.c_str();
}

size_t CatalogCount() noexcept { return kCatalogCount; }
const Catalog& CatalogAt(size_t index) noexcept { return kCatalogs[index]; }

const Catalog* Resolve(std::string_view setting, const std::vector<std::wstring>& uiLanguages) noexcept
{
    if (_strnicmp(setting.data(), kEnglish, setting.size()) == 0 && setting.size() == strlen(kEnglish)) return nullptr;
    if (!setting.empty() && setting != kAuto)
        if (const Catalog* catalog = FindCode(setting)) return catalog;
    // Auto. English UI languages fall through to the built-in English.
    for (const auto& language : uiLanguages)
    {
        if (PrefixMatches(language, kEnglish)) return nullptr;
        for (size_t index = 0; index < kCatalogCount; ++index)
            for (size_t i = 0; i < kCatalogs[index].matchCount; ++i)
                if (PrefixMatches(language, kCatalogs[index].matches[i])) return &kCatalogs[index];
    }
    return nullptr;
}

const Catalog* AutoCatalog() noexcept
{
    static const Catalog* const detected = Resolve(kAuto, PreferredUiLanguages());
    return detected;
}

void Select(std::string_view setting)
{
    gSelection = setting.empty() ? kAuto : std::string(setting);
    if (gSelection != kAuto && gSelection != kEnglish && !FindCode(gSelection)) gSelection = kAuto;
    gSelected = gSelection == kAuto ? AutoCatalog() : Resolve(gSelection, {});
}

const std::string& Selection() noexcept { return gSelection; }
const Catalog* Selected() noexcept { return gSelected; }

void SetDisplayed(const Catalog* catalog)
{
    if (catalog == gDisplayed) return;
    gDisplayed = catalog;
    gDisplayedUsable = catalog ? &Usable(*catalog) : nullptr;
    gLabels.clear();
}

const Catalog* Displayed() noexcept { return gDisplayed; }
}
