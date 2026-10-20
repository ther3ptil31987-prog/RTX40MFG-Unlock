"""generate_locale.py: format signatures (shared cases with the C++ runtime),
catalog rejections, extraction, update/merge and deterministic output."""
import argparse
import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile

sys.dont_write_bytecode = True  # never leave __pycache__ in source/native

parser = argparse.ArgumentParser()
parser.add_argument('--native', type=pathlib.Path, required=True)
parser.add_argument('--cases', type=pathlib.Path, required=True)
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('generate_locale', args.native / 'generate_locale.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)

checks = failures = 0


def quiet(function, *arguments):
    # update/check print progress and their own FAIL lines; only this test's results go to the log.
    with contextlib.redirect_stdout(io.StringIO()):
        return function(*arguments)


def check(condition, what):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print('FAIL ' + what)


HEADER = ('msgid ""\nmsgstr ""\n"Language: {language}\\n"\n"Content-Type: text/plain; charset=UTF-8\\n"\n'
          '"X-MFG-Native-Name: {native}\\n"\n"X-MFG-English-Name: Test\\n"\n{extra}\n')


def catalog_text(body, language='zz_ZZ', native='Zed', extra=''):
    return HEADER.format(language=language, native=native, extra=extra) + body


def rejects(name, text, what, raw=None):
    with tempfile.TemporaryDirectory() as folder:
        path = pathlib.Path(folder) / name
        if raw is not None:
            path.write_bytes(raw)
        else:
            path.write_text(text, encoding='utf-8', newline='\n')
        try:
            g.load_catalog(path)
        except g.CatalogError:
            check(True, what)
            return
        check(False, what + ' (accepted)')


def loads(name, text):
    with tempfile.TemporaryDirectory() as folder:
        path = pathlib.Path(folder) / name
        path.write_text(text, encoding='utf-8', newline='\n')
        return g.load_catalog(path)


# Shared format signature cases.
count = 0
for line in args.cases.read_text(encoding='utf-8').splitlines():
    if not line or line.startswith('#'):
        continue
    text, expected = line.split('\t', 1)
    text, expected = text.replace('\\n', '\n'), '' if expected == '-' else expected
    actual = g.format_signature(text)
    check(actual == expected, f'signature of {text!r}: {actual!r}, expected {expected!r}')
    count += 1
check(count >= 10, 'format cases read')

# A valid catalog; fuzzy and empty entries are not compiled.
valid = loads('zz-ZZ.po', catalog_text(
    'msgid "Off"\nmsgstr "Zoff"\n\n#, fuzzy\nmsgid "On"\nmsgstr "Zon"\n\nmsgid "Idle"\nmsgstr ""\n\n'
    '#, c-format\nmsgid "Preset %s"\nmsgstr "Z %s"\n', extra='"X-MFG-Fonts: msyh.ttc:1, simsun.ttc\\n"\n'))
check(valid['translations'] == {'Off': 'Zoff', 'Preset %s': 'Z %s'}, 'fuzzy and untranslated entries skipped')
check(valid['fonts'] == [('msyh.ttc', 1), ('simsun.ttc', 0)], 'font list with default face')
check(valid['matches'] == ['zz-ZZ'], 'match defaults to the code')

rejects('zz-ZZ.po', catalog_text('msgid "Preset %s"\nmsgstr "Z %d"\n'), 'placeholder type mismatch')
rejects('zz-ZZ.po', catalog_text('msgid "%s of %d"\nmsgstr "%d / %s"\n'), 'placeholder order mismatch')
rejects('zz-ZZ.po', catalog_text('msgid "%s"\nmsgstr "Z"\n'), 'dropped placeholder')
rejects('zz-ZZ.po', catalog_text('msgid "Done"\nmsgstr "100%"\n'), 'stray percent in a translation')
rejects('zz-ZZ.po', None, 'byte order mark', raw=b'\xef\xbb\xbf' + catalog_text('').encode())
rejects('zz-ZZ.po', None, 'invalid UTF-8', raw=catalog_text('msgid "Off"\nmsgstr "').encode() + b'\xff"\n')
rejects('zz-ZZ.po', catalog_text('msgid "Off"\nmsgstr "A"\n\nmsgid "Off"\nmsgstr "B"\n'), 'duplicate msgid')
rejects('zz-ZZ.po', catalog_text('msgid "Off"\nmsgid_plural "Offs"\nmsgstr[0] "A"\n'), 'plural forms')
rejects('zz-ZZ.po', catalog_text('msgctxt "menu"\nmsgid "Off"\nmsgstr "A"\n'), 'message context')
rejects('zz_ZZ.po', catalog_text(''), 'file name is not a BCP 47 code')
rejects('en.po', catalog_text('', language='en'), 'English catalog')
rejects('zz-ZZ.po', catalog_text('', language='zz_YY'), 'Language header differs from the file name')
rejects('zz-ZZ.po', catalog_text('', native=''), 'missing native name')
rejects('zz-ZZ.po', catalog_text('').replace('charset=UTF-8', 'charset=GBK'), 'non UTF-8 charset')
rejects('zz-ZZ.po', catalog_text('', extra='"X-MFG-Fonts: ..\\\\evil.ttf\\n"\n'), 'font outside the Fonts folder')
rejects('zz-ZZ.po', catalog_text('', extra='"X-MFG-Fonts: msyh.ttc:x\\n"\n'), 'non-numeric face')
rejects('zz-ZZ.po', catalog_text('msgid "Smile"\nmsgstr "\U0001F600"\n'), 'character outside the BMP')
rejects('zz-ZZ.po', catalog_text('msgid "Off"\nmsgstr "Z##hidden"\n'), '## in a translation')
rejects('zz-ZZ.po', catalog_text('msgid "Off"\nmsgstr "\\q"\n'), 'unknown escape')

# Extraction.
with tempfile.TemporaryDirectory() as folder:
    source = pathlib.Path(folder)
    (source / 'menu.cpp').write_text(
        '#include "localization.h"\n'
        '// T("comment") is ignored\n'
        'const char* a = T("Joined " "text");\n'
        'const char* b = l10n::T("Qualified %s");\n'
        'const char* c = l10n::Label("Header");\n'
        'const char* d[] = {N_("Stored")};\n'
        'const char* e = T(d[0]);\n'
        'const char* f = "T(\\"in a string\\")";\n'
        'const char* h = object.T("member");\n'
        'const char* i = T("Line\\n" "break");\n', encoding='utf-8')
    (source / 'other.cpp').write_text('const char* x = T("not localized file");\n', encoding='utf-8')
    found = g.extract(source)
    check(list(found) == ['Joined text', 'Qualified %s', 'Header', 'Stored', 'Line\nbreak'],
          f'extraction found {list(found)!r}')
    (source / 'bad.cpp').write_text('#include "localization.h"\nauto s = T("Hello " NAME);\n', encoding='utf-8')
    try:
        g.extract(source)
        check(False, 'a macro inside T() is rejected')
    except g.CatalogError:
        check(True, 'a macro inside T() is rejected')

# update: new strings added, removed ones kept as obsolete, returning ones reused (fuzzy).
with tempfile.TemporaryDirectory() as folder:
    root = pathlib.Path(folder)
    source, locale = root / 'src', root / 'locale'
    source.mkdir()
    locale.mkdir()
    menu = source / 'menu.cpp'
    menu.write_text('#include "localization.h"\nT("Off"); T("On"); T("Preset %s");\n', encoding='utf-8')
    (locale / 'zz-ZZ.po').write_text(catalog_text('msgid "Off"\nmsgstr "Zoff"\n\nmsgid "On"\nmsgstr "Zon"\n'),
                                     encoding='utf-8', newline='\n')
    quiet(g.update, locale, source)
    merged = g.load_catalog(locale / 'zz-ZZ.po')
    check(merged['entries'] == ['Off', 'On', 'Preset %s'], 'update adds new strings in source order')
    text = (locale / 'zz-ZZ.po').read_text(encoding='utf-8')
    check('#: ' not in text and 'c-format' not in text, 'catalogs carry no source references or c-format flags')
    check('#: menu.cpp' in (locale / 'messages.pot').read_text(encoding='utf-8'), 'the template keeps source references')
    check(quiet(g.check, locale, source, False) == 0, 'check passes after update')
    check(quiet(g.check, locale, source, True) == 1, 'require-complete fails on untranslated strings')
    menu.write_text('#include "localization.h"\nT("Off"); T("Preset %s");\n', encoding='utf-8')
    quiet(g.update, locale, source)
    text = (locale / 'zz-ZZ.po').read_text(encoding='utf-8')
    check('#~ msgid "On"' in text and '#~ msgstr "Zon"' in text, 'removed string kept as obsolete')
    check(g.load_catalog(locale / 'zz-ZZ.po')['entries'] == ['Off', 'Preset %s'], 'obsolete entries not compiled')
    menu.write_text('#include "localization.h"\nT("Off"); T("On"); T("Preset %s");\n', encoding='utf-8')
    quiet(g.update, locale, source)
    _, entries, obsolete, _ = g.parse_po(locale / 'zz-ZZ.po')
    returned = [e for e in entries if e['msgid'] == 'On']
    check(returned and returned[0]['msgstr'] == 'Zon' and 'fuzzy' in returned[0]['flags'] and not obsolete,
          'returning string reuses its translation, marked fuzzy')
    before = (locale / 'zz-ZZ.po').read_text(encoding='utf-8')
    quiet(g.update, locale, source)
    check((locale / 'zz-ZZ.po').read_text(encoding='utf-8') == before, 'update is stable')
    menu.write_text('#include "localization.h"\nT("Off"); T("On"); T("Preset %s"); T("New");\n', encoding='utf-8')
    check(quiet(g.check, locale, source, False) == 1, 'check fails when a catalog misses a new string')

# Generated table: deterministic, octal-escaped, sorted by hash.
shipped = g.load_catalogs(args.native / 'locale')
first, second = g.generate(shipped), g.generate(shipped)
check(first == second, 'generation is deterministic')
check(all(ord(c) < 0x80 for c in first), 'generated table is ASCII (octal escapes)')
check('"\\347\\212\\266\\346\\200\\201"' in first, 'UTF-8 translation emitted as octal bytes')
check(g.c_literal('a?b') == '"a\\077b"', 'question marks escaped')
check(g.fnv1a('a') == 0xE40C292C and g.fnv1a('') == 0x811C9DC5, 'FNV-1a reference values')
tables, current = [], None
for line in first.splitlines():
    if line.startswith('constexpr Entry '):
        current = []
        tables.append(current)
    elif line == '};':
        current = None
    elif current is not None and line.startswith('    {0x'):
        current.append(int(line.split('0x')[1][:8], 16))
check(len(tables) == len(shipped) and all(len(t) >= 100 and t == sorted(t) for t in tables),
      'each catalog table sorted by hash')

print(f'{"FAIL" if failures else "PASS"}: {checks} checks, {failures} failures')
raise SystemExit(1 if failures else 0)
