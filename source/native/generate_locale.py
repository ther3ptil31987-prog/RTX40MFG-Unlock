"""Menu translation catalogs (locale/<code>.po) for MFG Unlocker.

  generate  Validate every catalog and write locale_catalogs.generated.inc
            (run by CMake at configure time; any error fails the build).
  update    Extract T("..."), N_("...") and Label("...") strings from the
            sources and merge them into every catalog: new strings are added
            untranslated, unused ones are kept as obsolete (#~) entries. Also
            writes locale/messages.pot (not tracked) as a template for a new
            language.
  check     Report each catalog's untranslated and obsolete strings; fail if
            a catalog does not list the current strings or is invalid.
            --require-complete also fails on untranslated strings.

See locale/README.md for adding a language.
"""
import argparse
import pathlib
import re
import sys

ENCODING = 'utf-8'
SOURCE_SUFFIXES = {'.cpp', '.h', '.inl'}
KEYWORDS = ('T', 'N_', 'Label')
REQUIRED_HEADERS = ('Language', 'X-MFG-Native-Name', 'X-MFG-English-Name')


class CatalogError(Exception):
    pass


# ---------------------------------------------------------------- formats
LENGTHS = ('I64', 'I32', 'hh', 'll', 'h', 'l', 'j', 'z', 't', 'L', 'I')


def format_signature(text):
    """Same rule as l10n::FormatSignature in localization.cpp."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text[i] != '%':
            i += 1
            continue
        i += 1
        if i < n and text[i] == '%':
            i += 1
            continue
        token = '%'
        while i < n and text[i] in '-+ #0':
            i += 1
        if i < n and text[i] == '*':
            token += '*'
            i += 1
        else:
            while i < n and text[i].isdigit():
                i += 1
        if i < n and text[i] == '.':
            i += 1
            if i < n and text[i] == '*':
                token += '.*'
                i += 1
            else:
                while i < n and text[i].isdigit():
                    i += 1
        for length in LENGTHS:
            if text.startswith(length, i):
                token += length
                i += len(length)
                break
        if i >= n:
            out.append(token + '?')
            break
        out.append(token + text[i])
        i += 1
    return ''.join(t + ';' for t in out)


def fnv1a(text):
    value = 2166136261
    for byte in text.encode(ENCODING):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


# ---------------------------------------------------------------- .po files
PO_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}


def po_unquote(text, where):
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        raise CatalogError(f'{where}: expected a quoted string')
    body, out, i = text[1:-1], [], 0
    while i < len(body):
        c = body[i]
        if c == '\\':
            i += 1
            if i >= len(body) or body[i] not in PO_ESCAPES:
                raise CatalogError(f'{where}: unsupported escape')
            out.append(PO_ESCAPES[body[i]])
        elif c == '"':
            raise CatalogError(f'{where}: unescaped quote')
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def po_quote(text):
    escaped = text.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r')
    lines = escaped.split('\n')
    pieces = [line + '\\n' for line in lines[:-1]] + ([lines[-1]] if lines[-1] else [])
    if len(pieces) <= 1:
        return '"' + (pieces[0] if pieces else '') + '"'
    return '""\n' + '\n'.join('"' + piece + '"' for piece in pieces)


def parse_po(path):
    """Returns (headers, entries, obsolete); entries: list of dicts in file order."""
    raw = path.read_bytes()
    if raw.startswith(b'\xef\xbb\xbf'):
        raise CatalogError(f'{path.name}: remove the UTF-8 byte order mark')
    try:
        text = raw.decode(ENCODING)
    except UnicodeDecodeError as error:
        raise CatalogError(f'{path.name}: not valid UTF-8 ({error})') from None
    entries, obsolete, current, field, obsolete_field = [], [], None, None, 'msgid'

    def finish():
        nonlocal current, field
        if current is not None:
            if 'msgid' not in current or 'msgstr' not in current:
                raise CatalogError(f'{path.name}:{current["line"]}: entry needs msgid and msgstr')
            entries.append(current)
        current, field = None, None

    for number, line in enumerate(text.splitlines(), 1):
        where = f'{path.name}:{number}'
        stripped = line.strip()
        if not stripped:
            finish()
            continue
        if stripped.startswith('#~'):
            finish()
            body = stripped[2:].strip()
            if body.startswith('msgid '):
                obsolete.append({'msgid': po_unquote(body[6:].strip(), where), 'msgstr': ''})
                obsolete_field = 'msgid'
            elif body.startswith('msgstr ') and obsolete:
                obsolete[-1]['msgstr'] = po_unquote(body[7:].strip(), where)
                obsolete_field = 'msgstr'
            elif body.startswith('"') and obsolete:
                obsolete[-1][obsolete_field] += po_unquote(body, where)
            continue
        if stripped.startswith('#'):
            if current is not None and field is not None:
                finish()
            if current is None:
                current = {'line': number, 'flags': set(), 'comments': []}
            if stripped.startswith('#,'):
                current['flags'].update(flag.strip() for flag in stripped[2:].split(',') if flag.strip())
            elif not stripped.startswith('#:'):
                current['comments'].append(line)
            continue
        if current is None:
            current = {'line': number, 'flags': set(), 'comments': []}
        keyword, _, rest = stripped.partition(' ')
        if keyword in ('msgid', 'msgstr'):
            if keyword in current:
                raise CatalogError(f'{where}: repeated {keyword}')
            field = keyword
            current[field] = po_unquote(rest.strip(), where)
        elif keyword in ('msgctxt', 'msgid_plural') or keyword.startswith('msgstr['):
            raise CatalogError(f'{where}: {keyword} is not supported; use a whole sentence per entry')
        elif stripped.startswith('"') and field:
            current[field] += po_unquote(stripped, where)
        else:
            raise CatalogError(f'{where}: unexpected line')
    finish()
    if not entries or entries[0]['msgid'] != '':
        raise CatalogError(f'{path.name}: the first entry must be the header (msgid "")')
    headers = {}
    for line in entries[0]['msgstr'].split('\n'):
        if line.strip():
            name, colon, value = line.partition(':')
            if not colon:
                raise CatalogError(f'{path.name}: header line without a colon: {line!r}')
            headers[name.strip()] = value.strip()
    return headers, entries[1:], obsolete, entries[0]


def load_catalog(path):
    headers, entries, _, _ = parse_po(path)
    code = path.stem
    if not re.fullmatch(r'[a-z]{2,3}(-[A-Za-z0-9]{2,8})*', code):
        raise CatalogError(f'{path.name}: the file name must be a BCP 47 code such as zh-CN.po')
    if code.lower() == 'en':
        raise CatalogError(f'{path.name}: English is the source language, not a catalog')
    for name in REQUIRED_HEADERS:
        if not headers.get(name):
            raise CatalogError(f'{path.name}: missing header {name}')
    if headers['Language'].replace('_', '-').lower() != code.lower():
        raise CatalogError(f'{path.name}: Language header {headers["Language"]!r} does not match the file name')
    if 'charset=UTF-8' not in headers.get('Content-Type', ''):
        raise CatalogError(f'{path.name}: Content-Type must declare charset=UTF-8')
    matches = headers.get('X-MFG-Match', code).replace(',', ' ').split()
    for match in matches:
        if not re.fullmatch(r'[A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*', match):
            raise CatalogError(f'{path.name}: bad X-MFG-Match prefix {match!r}')
    fonts = []
    for item in headers.get('X-MFG-Fonts', '').replace(',', ' ').split():
        name, _, face = item.partition(':')
        if not re.fullmatch(r'[A-Za-z0-9_.-]+\.(ttf|ttc|otf)', name, re.IGNORECASE) or (face and not face.isdigit()):
            raise CatalogError(f'{path.name}: bad X-MFG-Fonts item {item!r} (file.ttc:face, Windows Fonts folder only)')
        fonts.append((name, int(face or 0)))
    translations, seen = {}, set()
    for entry in entries:
        english, translated = entry['msgid'], entry['msgstr']
        where = f'{path.name}:{entry["line"]}'
        if english in seen:
            raise CatalogError(f'{where}: duplicate msgid {english!r}')
        seen.add(english)
        if not english:
            raise CatalogError(f'{where}: empty msgid')
        if not translated or 'fuzzy' in entry['flags']:
            continue
        if format_signature(english) != format_signature(translated):
            raise CatalogError(f'{where}: placeholders differ from the English text '
                               f'({format_signature(english) or "none"} vs {format_signature(translated) or "none"}): {english!r}')
        if '##' in translated and '##' not in english:
            raise CatalogError(f'{where}: "##" in a translation would hide text in ImGui labels')
        translations[english] = translated
    glyphs = sorted({ord(c) for text in list(translations.values()) + [headers['X-MFG-Native-Name']]
                     for c in text if ord(c) > 0x7F})
    if glyphs and glyphs[-1] > 0xFFFF:
        raise CatalogError(f'{path.name}: U+{glyphs[-1]:X} is outside the Basic Multilingual Plane, which the menu font cannot draw')
    return {'code': code, 'native': headers['X-MFG-Native-Name'], 'english': headers['X-MFG-English-Name'],
            'matches': matches, 'fonts': fonts, 'translations': translations, 'glyphs': glyphs,
            'entries': [e['msgid'] for e in entries]}


def load_catalogs(locale):
    paths = sorted(p for p in locale.glob('*.po'))
    catalogs = [load_catalog(p) for p in paths]
    codes = [c['code'].lower() for c in catalogs]
    if len(set(codes)) != len(codes):
        raise CatalogError('two catalogs share a language code')
    return catalogs


# ---------------------------------------------------------------- generate
def c_literal(text):
    out = []
    for byte in text.encode(ENCODING):
        c = chr(byte)
        if c == '\\':
            out.append('\\\\')
        elif c == '"':
            out.append('\\"')
        elif c == '\n':
            out.append('\\n')
        elif c == '\t':
            out.append('\\t')
        elif 0x20 <= byte < 0x7F and c != '?':
            out.append(c)
        else:
            out.append(f'\\{byte:03o}')
    return '"' + ''.join(out) + '"'


def generate(catalogs):
    lines = ['// Generated by generate_locale.py from locale/*.po. Do not edit.', '']
    for index, catalog in enumerate(catalogs):
        prefix = f'kCatalog{index}'
        items = sorted(catalog['translations'].items(), key=lambda kv: (fnv1a(kv[0]), kv[0]))
        lines.append(f'// {catalog["code"]}: {len(items)} translations')
        lines.append(f'constexpr Entry {prefix}Entries[] = {{')
        for english, translated in items:
            lines.append(f'    {{0x{fnv1a(english):08X}u, {c_literal(english)}, {c_literal(translated)}}},')
        if not items:
            lines.append('    {0u, "", ""},')
        lines.append('};')
        lines.append(f'constexpr const char* {prefix}Matches[] = {{' + ', '.join(c_literal(m) for m in catalog['matches']) + '};')
        fonts = ', '.join(f'{{L{c_literal(name)}, {face}u}}' for name, face in catalog['fonts']) or '{nullptr, 0u}'
        lines.append(f'constexpr FontFace {prefix}Fonts[] = {{{fonts}}};')
        glyphs = catalog['glyphs'] or [0]
        lines.append(f'constexpr char16_t {prefix}Glyphs[] = {{')
        for start in range(0, len(glyphs), 16):
            lines.append('    ' + ', '.join(f'0x{g:04X}' for g in glyphs[start:start + 16]) + ',')
        lines.append('};')
        lines.append('')
    lines.append(f'constexpr size_t kCatalogCount = {len(catalogs)};')
    lines.append(f'constexpr Catalog kCatalogs[{max(1, len(catalogs))}] = {{')
    for index, catalog in enumerate(catalogs):
        prefix = f'kCatalog{index}'
        lines.append(f'    {{{c_literal(catalog["code"])}, {c_literal(catalog["native"])}, {c_literal(catalog["english"])},')
        lines.append(f'        {prefix}Matches, {len(catalog["matches"])}, {prefix}Fonts, {len(catalog["fonts"])},')
        lines.append(f'        {prefix}Entries, {len(catalog["translations"])}, {prefix}Glyphs, {len(catalog["glyphs"])}}},')
    if not catalogs:
        lines.append('    {},')
    lines.append('};')
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------- extraction
C_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'", 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}


def decode_c_literal(body, where):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        i += 1
        e = body[i]
        if e in '01234567':
            j = i
            while j < len(body) and j < i + 3 and body[j] in '01234567':
                j += 1
            out.append(chr(int(body[i:j], 8)))
            i = j
        elif e == 'x':
            j = i + 1
            while j < len(body) and body[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(chr(int(body[i + 1:j], 16)))
            i = j
        elif e in C_ESCAPES:
            out.append(C_ESCAPES[e])
            i += 1
        else:
            raise CatalogError(f'{where}: unsupported escape \\{e}')
    return ''.join(out)


TOKEN = re.compile(r'''
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>(?:u8)?"(?:[^"\\\n]|\\.)*")
  | (?P<raw>R"(?P<delim>[^(\s]*)\(.*?\)(?P=delim)")
  | (?P<char>'(?:[^'\\\n]|\\.)+')
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<punct>[(),])
  | (?P<space>\s+)
  | (?P<other>.)
''', re.VERBOSE | re.DOTALL)


def extract_file(path, root):
    text = path.read_text(encoding=ENCODING)
    if 'localization.h' not in text:
        return []
    tokens = [(m.lastgroup, m.group(), m.start()) for m in TOKEN.finditer(text)
              if m.lastgroup not in ('comment', 'space')]
    found = []
    relative = path.relative_to(root).as_posix()
    for i, (kind, value, offset) in enumerate(tokens):
        if kind != 'ident' or value not in KEYWORDS or i + 2 >= len(tokens) or tokens[i + 1][1] != '(':
            continue
        if i and tokens[i - 1][1] in ('.', '>'):
            continue
        j, pieces = i + 2, []
        while j < len(tokens) and tokens[j][0] == 'string':
            literal = tokens[j][1]
            pieces.append(literal[3:-1] if literal.startswith('u8') else literal[1:-1])
            j += 1
        if not pieces:
            continue  # T(variable): translates a string marked elsewhere.
        line = text.count('\n', 0, offset) + 1
        where = f'{relative}:{line}'
        if j >= len(tokens) or tokens[j][1] not in (')', ','):
            raise CatalogError(f'{where}: {value}() takes string literals only (use "%s" for macros or values)')
        found.append((decode_c_literal(''.join(pieces), where), relative))
    return found


def extract(source):
    messages = {}
    for path in sorted(source.rglob('*')):
        if path.suffix not in SOURCE_SUFFIXES or 'third_party' in path.parts or not path.is_file():
            continue
        for message, relative in extract_file(path, source):
            messages.setdefault(message, [])
            if relative not in messages[message]:
                messages[message].append(relative)
    return messages


# ---------------------------------------------------------------- update/check
TEMPLATE_HEADER = ('Project-Id-Version: MFG Unlocker\\n"\n"Language: \\n"\n'
                   '"MIME-Version: 1.0\\n"\n"Content-Type: text/plain; charset=UTF-8\\n"\n'
                   '"Content-Transfer-Encoding: 8bit\\n')


def render_template(messages):
    out = ['# MFG Unlocker menu text. Generated by generate_locale.py update; do not translate this file.',
           '# Copy it to <code>.po to start a language (see README.md).',
           'msgid ""', f'msgstr ""\n"{TEMPLATE_HEADER}"', '']
    for message, files in messages.items():
        out.append('#: ' + ' '.join(files))
        if format_signature(message):
            out.append('#, c-format')
        out.append('msgid ' + po_quote(message))
        out.append('msgstr ""')
        out.append('')
    return '\n'.join(out)


def render_catalog(header, messages, translations, flags, comments, obsolete):
    out = list(header['comments'])
    out.append('msgid ""')
    out.append('msgstr ' + po_quote(header['msgstr']))
    out.append('')
    # No '#:' source references or automatic c-format flags: the generator reads neither (placeholders are
    # checked from the strings themselves), and messages.pot keeps the references for translators.
    for message in messages:
        out.extend(comments.get(message, []))
        entry_flags = set(flags.get(message, set()))
        if entry_flags:
            out.append('#, ' + ', '.join(sorted(entry_flags)))
        out.append('msgid ' + po_quote(message))
        out.append('msgstr ' + po_quote(translations.get(message, '')))
        out.append('')
    for message, translated in obsolete.items():
        if not translated:
            continue
        for prefix, value in (('msgid', message), ('msgstr', translated)):
            quoted = po_quote(value).split('\n')
            out.append(f'#~ {prefix} {quoted[0]}')
            out.extend(f'#~ {line}' for line in quoted[1:])
        out.append('')
    return '\n'.join(out)


def write_if_changed(path, text):
    if path.exists() and path.read_text(encoding=ENCODING) == text:
        return False
    path.write_text(text, encoding=ENCODING, newline='\n')
    return True


def update(locale, source):
    messages = extract(source)
    changed = write_if_changed(locale / 'messages.pot', render_template(messages))
    print(f'messages.pot: {len(messages)} strings' + (' (updated)' if changed else ''))
    for path in sorted(locale.glob('*.po')):
        header_values, entries, old_obsolete, header = parse_po(path)
        translations = {e['msgid']: e['msgstr'] for e in entries}
        flags = {e['msgid']: e['flags'] - {'c-format'} for e in entries}
        comments = {e['msgid']: e['comments'] for e in entries}
        obsolete = {e['msgid']: e['msgstr'] for e in old_obsolete}
        obsolete.update({k: v for k, v in translations.items() if k not in messages})
        # A string that came back reuses its old translation, marked for review.
        for message in messages:
            if not translations.get(message) and obsolete.get(message):
                translations[message] = obsolete[message]
                flags[message] = flags.get(message, set()) | {'fuzzy'}
        obsolete = {k: v for k, v in obsolete.items() if k not in messages}
        text = render_catalog(header, messages, translations, flags, comments, obsolete)
        changed = write_if_changed(path, text)
        missing = sum(1 for m in messages if not translations.get(m))
        print(f'{path.name}: {len(messages) - missing}/{len(messages)} translated'
              + (f', {len(obsolete)} obsolete' if obsolete else '') + (' (updated)' if changed else ''))
        load_catalog(path)


def check(locale, source, require_complete):
    messages = extract(source)
    failures = []
    for catalog in load_catalogs(locale):
        untranslated = [m for m in messages if m not in catalog['translations']]
        unused = [m for m in catalog['translations'] if m not in messages]
        print(f'{catalog["code"]}: {len(messages) - len(untranslated)}/{len(messages)} translated, '
              f'{len(unused)} not used by the sources')
        for message in untranslated[:20]:
            print(f'  untranslated: {message!r}')
        if require_complete and untranslated:
            failures.append(f'{catalog["code"]}: {len(untranslated)} untranslated strings')
        if set(catalog['entries']) != set(messages):
            failures.append(f'{catalog["code"]}.po does not list the current strings: run generate_locale.py update')
    for failure in failures:
        print('FAIL ' + failure)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('generate', 'update', 'check'))
    parser.add_argument('--locale', type=pathlib.Path, required=True, help='directory holding <code>.po files')
    parser.add_argument('--output', type=pathlib.Path, help='generate: directory for locale_catalogs.generated.inc')
    parser.add_argument('--source', type=pathlib.Path, help='update/check: source tree to extract strings from')
    parser.add_argument('--require-complete', action='store_true')
    args = parser.parse_args()
    try:
        if args.command == 'generate':
            if not args.output:
                parser.error('generate needs --output')
            catalogs = load_catalogs(args.locale)
            args.output.mkdir(parents=True, exist_ok=True)
            write_if_changed(args.output / 'locale_catalogs.generated.inc', generate(catalogs))
            print('locale catalogs: ' + (', '.join(f'{c["code"]} ({len(c["translations"])})' for c in catalogs) or 'none'))
            return 0
        if not args.source:
            parser.error(f'{args.command} needs --source')
        if args.command == 'update':
            update(args.locale, args.source)
            return 0
        return check(args.locale, args.source, args.require_complete)
    except CatalogError as error:
        print(f'generate_locale.py: error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
