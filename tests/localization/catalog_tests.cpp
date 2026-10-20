// The shipped catalogs (source/native/locale/*.po as compiled into MFGUnlocker.dll):
// table layout, every translation usable at runtime, Auto rules for Chinese, and
// that the Windows fonts each catalog lists draw every glyph it uses, loaded the
// way PlatformState::CatalogFont does (Segoe UI + the first installed listed face
// merged, pinned ImGui font loader).
#include "localization.h"

#include <imgui.h>

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
int gFailures = 0;
int gChecks = 0;

bool Check(bool condition, const char* what, const char* detail = "")
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL %s %s\n", what, detail); }
    return condition;
}

const l10n::Catalog* Find(const char* code)
{
    for (size_t i = 0; i < l10n::CatalogCount(); ++i)
        if (!strcmp(l10n::CatalogAt(i).code, code)) return &l10n::CatalogAt(i);
    return nullptr;
}

void CheckFonts(const l10n::Catalog& catalog, const std::filesystem::path& fonts)
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    ImFontConfig config;
    config.Flags |= ImFontFlags_NoLoadError;
    const auto latin = (fonts/L"segoeui.ttf").u8string();
    ImFont* font = io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(latin.c_str()), 16.0f, &config);
    Check(font != nullptr, "Segoe UI loads", catalog.code);
    std::wstring used;
    for (size_t i = 0; font && i < catalog.fontCount && used.empty(); ++i)
    {
        const auto path = fonts/catalog.fonts[i].file;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        ImFontConfig merge;
        merge.Flags |= ImFontFlags_NoLoadError;
        merge.MergeMode = true;
        merge.DstFont = font;
        merge.FontNo = catalog.fonts[i].face;
        const auto file = path.u8string();
        if (io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(file.c_str()), 16.0f, &merge))
            used = catalog.fonts[i].file;
    }
    Check(!catalog.fontCount || !used.empty(), "a listed font is installed and loads", catalog.code);
    size_t missing = 0;
    for (size_t i = 0; font && i < catalog.glyphCount; ++i)
        if (!font->IsGlyphInFont(static_cast<ImWchar>(catalog.glyphs[i])))
        {
            if (++missing <= 10) std::printf("  %s: U+%04X not drawn\n", catalog.code, catalog.glyphs[i]);
        }
    Check(font && !missing, "the catalog font draws every glyph", catalog.code);
    std::printf("%s: %zu translations, %zu glyphs, font %ls\n", catalog.code, catalog.entryCount,
        catalog.glyphCount, used.empty() ? L"Segoe UI only" : used.c_str());
    ImGui::DestroyContext();
}
}

int main()
{
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    const std::filesystem::path fonts = length && length < MAX_PATH
        ? std::filesystem::path(windows)/L"Fonts" : std::filesystem::path();
    Check(!fonts.empty() && std::filesystem::exists(fonts/L"segoeui.ttf"), "Windows Fonts folder with Segoe UI");

    Check(l10n::CatalogCount() >= 1, "at least one catalog ships");
    for (size_t index = 0; index < l10n::CatalogCount(); ++index)
    {
        const auto& catalog = l10n::CatalogAt(index);
        Check(catalog.code[0] && catalog.nativeName[0] && catalog.englishName[0] && catalog.matchCount,
            "catalog header fields", catalog.code);
        bool ordered = true, hashes = true, usable = true;
        for (size_t i = 0; i < catalog.entryCount; ++i)
        {
            const auto& entry = catalog.entries[i];
            hashes = hashes && entry.hash == l10n::Fnv1a(entry.english);
            if (i) ordered = ordered && catalog.entries[i - 1].hash <= entry.hash;
            if (l10n::FormatSignature(entry.english) != l10n::FormatSignature(entry.translated))
            {
                usable = false;
                std::printf("  %s: placeholders differ for '%s'\n", catalog.code, entry.english);
            }
        }
        Check(hashes && ordered, "entries carry FNV-1a hashes in order", catalog.code);
        Check(usable, "runtime and generator agree on every placeholder", catalog.code);
        bool glyphs = true;
        for (size_t i = 0; i < catalog.glyphCount; ++i)
            glyphs = glyphs && catalog.glyphs[i] > 0x7F && (!i || catalog.glyphs[i - 1] < catalog.glyphs[i]);
        Check(glyphs, "glyph list sorted, unique and non-ASCII", catalog.code);

        l10n::SetDisplayed(&catalog);
        bool lookups = true;
        for (size_t i = 0; i < catalog.entryCount; ++i)
            lookups = lookups && l10n::T(catalog.entries[i].english) == catalog.entries[i].translated;
        Check(lookups, "every entry is found and used", catalog.code);
        l10n::SetDisplayed(nullptr);
        if (!fonts.empty()) CheckFonts(catalog, fonts);
    }

    // Auto picks each catalog for its own Windows languages only (also as a later preference). Only
    // Simplified Chinese lists a font (Microsoft YaHei UI first); Segoe UI draws the others.
    struct Expected
    {
        const char* code;
        std::vector<std::wstring> picks, others, later; // later: a preference list ending in the catalog's language
        const char* nativeName;
        const wchar_t* font;                             // first listed font file; nullptr = none listed
        uint32_t face;
        const char* diagnostics;                         // T("Diagnostics")
        bool label;                                      // Label() keeps the English ID
    };
    const Expected expected[] = {
        {"zh-CN", {L"zh-CN", L"zh-SG", L"zh-Hans", L"zh-Hans-CN"}, {L"zh-TW", L"zh-HK", L"zh-Hant-TW", L"zh-MO", L"ja-JP"}, {},
            "简体中文", L"msyh.ttc", 1, "诊断", true},
        {"uk-UA", {L"uk-UA", L"uk"}, {L"ru-RU", L"be-BY", L"pl-PL"}, {L"ru-RU", L"uk-UA"}, "Українська", nullptr, 0,
            "Діагностика", false},
        {"it-IT", {L"it-IT", L"it-CH", L"it"}, {L"es-ES", L"fr-FR", L"pt-BR", L"ro-RO"}, {}, "Italiano", nullptr, 0,
            "Diagnostica", true},
        {"sv-SE", {L"sv-SE", L"sv-FI", L"sv"}, {L"nb-NO", L"nn-NO", L"da-DK", L"fi-FI"}, {L"fi-FI", L"sv-FI"}, "Svenska",
            nullptr, 0, "Diagnostik", false},
        {"tr-TR", {L"tr-TR", L"tr-CY", L"tr"}, {L"az-Latn-AZ", L"el-GR", L"bg-BG"}, {L"az-Latn-AZ", L"tr-TR"}, "Türkçe",
            nullptr, 0, "Tanılama", true},
        {"vi-VN", {L"vi-VN", L"vi"}, {L"th-TH", L"km-KH", L"lo-LA", L"id-ID"}, {L"th-TH", L"vi-VN"}, "Tiếng Việt",
            nullptr, 0, "Chẩn đoán", true},
    };
    for (const auto& row : expected)
    {
        const l10n::Catalog* shipped = Find(row.code);
        if (!Check(shipped != nullptr, "catalog ships", row.code)) continue;
        for (const auto& language : row.picks)
            Check(l10n::Resolve(l10n::kAuto, {language}) == shipped, "Auto picks the catalog", row.code);
        for (const auto& language : row.others)
            Check(l10n::Resolve(l10n::kAuto, {language}) == nullptr, "Auto keeps English for neighbouring languages", row.code);
        if (!row.later.empty())
            Check(l10n::Resolve(l10n::kAuto, row.later) == shipped, "the catalog as a later preference", row.code);
        const bool listed = row.font ? shipped->fontCount >= 1 && !wcscmp(shipped->fonts[0].file, row.font)
            && shipped->fonts[0].face == row.face : shipped->fontCount == 0;
        Check(!strcmp(shipped->nativeName, row.nativeName) && listed, "header: native name and listed fonts", row.code);
        l10n::SetDisplayed(shipped);
        Check(!strcmp(l10n::T("Diagnostics"), row.diagnostics), "translation", row.code);
        if (row.label)
            Check(std::string(row.diagnostics) + "###Diagnostics" == l10n::Label("Diagnostics"), "label keeps the English ID", row.code);
        l10n::SetDisplayed(nullptr);
    }

    std::printf("%s: %d checks, %d failures\n", gFailures ? "FAIL" : "PASS", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
