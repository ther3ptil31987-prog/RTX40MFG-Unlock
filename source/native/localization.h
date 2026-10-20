#pragma once
// Menu localisation. English text is the lookup key: T("...") returns the
// displayed catalog's translation, or the English text when there is none.
// Catalogs are locale/<code>.po, compiled in by generate_locale.py (see
// locale/README.md). Diagnostics, logs, status JSON and ini keys stay English.
// UI-thread only (the overlay draws under gUiMutex).
#include "localization_catalog.h"

#include <string>
#include <string_view>
#include <vector>

// Marks a literal for extraction where it is stored and translated later with T().
#define N_(text) text

namespace l10n
{
// Settings values besides catalog codes. Empty or unknown values mean Auto.
constexpr const char* kAuto = "auto";
constexpr const char* kEnglish = "en";

const char* T(const char* english) noexcept;
// "translation###English" for labels that are also ImGui IDs (checkboxes,
// headers), so a language change keeps their ID and state.
const char* Label(const char* english);

size_t CatalogCount() noexcept;
const Catalog& CatalogAt(size_t index) noexcept;

// The user's choice: kAuto, kEnglish or a catalog code (as saved in RTX40MFG-UI.ini).
void Select(std::string_view setting);
const std::string& Selection() noexcept;
// The catalog the selection resolves to (Auto applied); nullptr = English.
const Catalog* Selected() noexcept;
// What Auto picks for this Windows user; nullptr = English.
const Catalog* AutoCatalog() noexcept;
// Auto's rule, for tests: the first preferred UI language (BCP 47, best first)
// that a catalog's match prefixes cover.
const Catalog* Resolve(std::string_view setting, const std::vector<std::wstring>& uiLanguages) noexcept;

// The catalog T() draws from this frame. The overlay sets it once it has a font
// that covers the catalog's glyphs, else English.
void SetDisplayed(const Catalog* catalog);
const Catalog* Displayed() noexcept;

// Printf conversions in order (flags, width and precision values excluded).
// A translation is only used when its signature equals the English one.
std::string FormatSignature(const char* text);
}
