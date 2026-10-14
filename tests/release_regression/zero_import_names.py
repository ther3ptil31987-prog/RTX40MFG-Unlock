"""Rewrite a freshly linked x64 executable to the Themida import layout: every
import descriptor keeps only FirstThunk (OriginalFirstThunk = 0). The Windows
loader then reads the import names from FirstThunk and binds it in place.

Usage: zero_import_names.py <executable>   (in place, post-build)
"""
import struct
import sys


def main(path):
    data = bytearray(open(path, 'rb').read())
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise SystemExit(f'{path}: not a PE image')
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from('<H', data, optional)[0] != 0x20B:
        raise SystemExit(f'{path}: not PE32+')
    directories = optional + 112
    if struct.unpack_from('<II', data, directories + 11 * 8) != (0, 0):
        raise SystemExit(f'{path}: bound imports present; the loader would skip the names')
    layout = []
    for index in range(sections):
        header = optional + optional_size + index * 40
        virtual_size, address, raw_size, raw_pointer = struct.unpack_from('<IIII', data, header + 8)
        layout.append((address, max(virtual_size, raw_size), raw_pointer, raw_size))

    def offset(rva):
        for address, size, raw_pointer, raw_size in layout:
            if address <= rva < address + size and rva - address < raw_size:
                return raw_pointer + rva - address
        raise SystemExit(f'{path}: RVA {rva:#x} is not in raw data')

    imports = struct.unpack_from('<I', data, directories + 8)[0]
    if not imports:
        raise SystemExit(f'{path}: no import directory')
    rewritten = 0
    for index in range(4096):
        descriptor = offset(imports + index * 20)
        names, _, _, name, slots = struct.unpack_from('<5I', data, descriptor)
        if not name:
            break
        if names:
            # Unbound on disk: FirstThunk must already carry the same name RVAs.
            for entry in range(4096):
                value = struct.unpack_from('<Q', data, offset(names + entry * 8))[0]
                if value != struct.unpack_from('<Q', data, offset(slots + entry * 8))[0]:
                    raise SystemExit(f'{path}: FirstThunk differs from the name table')
                if not value:
                    break
            struct.pack_into('<I', data, descriptor, 0)
        rewritten += 1
    if rewritten < 2:
        raise SystemExit(f'{path}: expected at least two import descriptors')
    open(path, 'wb').write(data)
    print(f'zero_import_names: {rewritten} descriptor(s) without OriginalFirstThunk in {path}')


if __name__ == '__main__':
    main(sys.argv[1])
