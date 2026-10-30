#!/usr/bin/env python3
"""Dump a standard flattened device tree (DTB) into readable text.

Usage: fdt-dump.py FILE.dtb [--flat] [--props NAME,...]
  default: prints the whole tree
  --flat : node path + key=value lines (grep-friendly)
"""
import struct, sys

def parse(path):
    b = open(path, 'rb').read()
    magic = struct.unpack_from('>I', b, 0)[0]
    assert magic == 0xd00dfeed, hex(magic)
    total, off_struct, off_strings, off_rsvmap, ver, last_comp, boot_cpu, size_strings, size_struct = struct.unpack_from('>9I', b, 4)
    return b, off_struct, off_strings, size_strings

def walk(b, off_struct, off_strings, size_strings):
    nodes = []  # list of (path, [(name, bytes)])
    path = []
    p = off_struct
    while True:
        tok = struct.unpack_from('>I', b, p)[0]
        p += 4
        if tok == 0x1:  # BEGIN_NODE
            end = b.index(b'\0', p)
            name = b[p:end].decode(errors='replace')
            p = (end + 4) & ~3
            path.append(name)
            nodes.append(('/'.join(path), []))
        elif tok == 0x2:  # END_NODE
            path.pop()
        elif tok == 0x3:  # PROP
            ln, nameoff = struct.unpack_from('>II', b, p)
            p += 8
            val = b[p:p + ln]
            p = (p + ln + 3) & ~3
            end = b.index(b'\0', off_strings + nameoff)
            name = b[off_strings + nameoff:end].decode(errors='replace')
            nodes[-1][1].append((name, val))
        elif tok == 0x4:  # NOP
            continue
        elif tok == 0x9:  # END
            break
        else:
            raise SystemExit(f'unknown token {tok:#x} at {p:#x}')
    return nodes

def fmt(v):
    if not v:
        return '""'
    if v[-1:] == b'\0' and all(32 <= c < 127 for c in v[:-1]):
        s = v.rstrip(b'\0').decode(errors='replace')
        return repr(s)
    if len(v) == 4:
        return '0x%08x' % struct.unpack_from('>I', v)[0]
    if len(v) == 8:
        return '0x%016x' % struct.unpack_from('>Q', v)[0]
    if len(v) % 4 == 0:
        return '<' + ' '.join(hex(x) for x in struct.unpack('>%dI' % (len(v) // 4), v)) + '>'
    return v.hex()

def main():
    b, off_s, off_str, size_str = parse(sys.argv[1])
    nodes = walk(b, off_s, off_str, size_str)
    flat = '--flat' in sys.argv
    only = None
    if '--props' in sys.argv:
        only = sys.argv[sys.argv.index('--props') + 1].split(',')
    for path, props in nodes:
        if only:
            props = [(n, v) for n, v in props if any(o in n for o in only)]
            if not props:
                continue
        if flat:
            for n, v in props:
                print(f'{path}\t{n} = {fmt(v)}')
        else:
            print(f'{path} {{')
            for n, v in props:
                print(f'    {n} = {fmt(v)}')
            print('}')

if __name__ == '__main__':
    main()
