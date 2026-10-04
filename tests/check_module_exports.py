#!/usr/bin/env python3
"""Check a module's ELF imports against an older kernel's exported symbols.

Capture exports with: adb shell su -c 'grep __ksymtab_ /proc/kallsyms'
Usage: python3 tests/check_module_exports.py module.ko kernel-exports.txt
"""
import argparse
from pathlib import Path
import struct


def undefined_symbols(path):
    data = Path(path).read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected a little-endian ELF64 kernel module")
    offset = struct.unpack_from("<Q", data, 40)[0]
    entry_size, count = struct.unpack_from("<HH", data, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * entry_size)
                for i in range(count)]
    imports = set()
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for position in range(section[4], section[4] + section[5], section[9]):
            name, info, _, index, _, _ = struct.unpack_from("<IBBHQQ", data, position)
            if name and index == 0 and info >> 4 != 2:  # strong SHN_UNDEF only
                imports.add(names[name:names.index(b"\0", name)].decode())
    if not imports:
        raise ValueError("no strong imports found; expected a kernel module")
    return imports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module")
    parser.add_argument("exports", help="filtered /proc/kallsyms from the target kernel")
    args = parser.parse_args()
    exports = set()
    for line in Path(args.exports).read_text(encoding="utf-8-sig").splitlines():
        words = line.split()
        if len(words) >= 3 and words[2].startswith("__ksymtab_"):
            exports.add(words[2][len("__ksymtab_"):])
    if not exports:
        parser.error("no __ksymtab_ exports found")
    missing = sorted(undefined_symbols(args.module) - exports)
    if missing:
        print("FAIL: kernel does not export: " + ", ".join(missing))
        return 1
    print("PASS: all strong module imports are exported by the target kernel")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
