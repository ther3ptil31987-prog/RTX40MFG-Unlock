# Menu translations

The in-game menu reads its text from the catalogs in this folder. Each
`<code>.po` file is one language, in the standard gettext format, so Poedit,
Weblate or any text editor can open it. Every catalog is compiled into
`MFGUnlocker.dll`, so the release stays a single file.

Shipped: `zh-CN.po` (Simplified Chinese, Microsoft YaHei UI), `uk-UA.po`
(Ukrainian, Segoe UI), `it-IT.po` (Italian, Segoe UI), `sv-SE.po` (Swedish,
Segoe UI), `tr-TR.po` (Turkish, Segoe UI) and `vi-VN.po` (Vietnamese, Segoe UI).

English is the source text, not a catalog. Anything left untranslated in a
catalog shows in English. Diagnostics, logs, status files and ini keys stay in
English, so bug reports remain readable.

## Choosing the language in game

The language list sits in the menu footer, next to the menu key. **Auto**
follows the Windows display language. The choice is saved in
`RTX40MFG-UI.ini` (`[Overlay] Language = auto | en | <code>`) next to the game.

## Adding a language

1. Run `python generate_locale.py update --locale locale --source .` (see below)
   to write `messages.pot`, the template (it is not tracked). Copy it to
   `<code>.po`, where `<code>` is a BCP 47 tag such as
   `ja-JP`, `ko-KR`, `de-DE` or `zh-TW`.
2. Fill in the header:

   | Header | Meaning |
   | --- | --- |
   | `Language` | The same code as the file name (`_` or `-`). |
   | `X-MFG-Native-Name` | The language's name in itself, shown in the list (`日本語`). |
   | `X-MFG-English-Name` | Shown instead when no loaded font can draw the native name. |
   | `X-MFG-Match` | Optional. Windows display languages that Auto picks this catalog for, as whole-subtag prefixes (`ja`, `zh-TW zh-HK zh-Hant`). Default: the code. |
   | `X-MFG-Fonts` | Optional. Fonts from the Windows `Fonts` folder for scripts Segoe UI cannot draw, as `file:face`, tried in order (`YuGothR.ttc:0 meiryo.ttc:0`). No font is ever shipped. |

3. Translate each `msgstr`. Keep every `%s`, `%d`, `%u`, `%ux`, `%.1f` and
   similar placeholder, in the same order. The build fails otherwise, because a
   mismatched placeholder would crash the game. Keep `\n` line breaks in tooltips.
   Segoe UI already draws Latin, Cyrillic and Greek, so those catalogs list no font.
   Keep grid labels short: the label column grows to fit them, up to 14 em.
   Where a word would change with the number, write `label: %u` rather than
   inflecting around the placeholder.
4. Rebuild. The catalog is validated and compiled in automatically.

If a catalog uses a character that none of its listed fonts can draw, the menu
stays in English for that player, and the log notes it, so they never see
missing-glyph boxes.

## When the menu text changes

Wrap new menu text as `T("...")`, use `l10n::Label("...")` for checkbox and
header labels, or `N_("...")` where a string is stored and translated later.
Pass placeholders instead of joining sentences from pieces. Then run:

```
python generate_locale.py update --locale locale --source .
python generate_locale.py check --locale locale --source .
```

`update` writes `messages.pot` and adds the new strings to every catalog,
untranslated. Removed strings are kept as obsolete `#~` entries so they can be
reused. `check` lists what each catalog still needs to translate.
