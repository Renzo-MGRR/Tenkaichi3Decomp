#!/usr/bin/env python3
"""Write the little-endian .dat list that loads game tables from the user's own disc."""
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = ROOT / "port/data"
DATA_OBJ = ROOT / "port/build/obj_ps3_data"
HAL = ROOT / "port/build/ps3"
EXE = HAL / "bt3.elf"
MAP = HAL / "bt3.map"


def index():
    where, checksum = {}, None
    for line in (DATA / "index.txt").read_text().splitlines():
        parts = line.split()
        if len(parts) == 3 and not line.startswith("#"):
            where[parts[0]] = (int(parts[1]), int(parts[2], 16))
        elif len(parts) == 2 and parts[0] == "checksum":
            checksum = int(parts[1], 16)
    if checksum is None:
        raise ValueError("port/data/index.txt has no checksum")
    return where, checksum


def placed_objects():
    pattern = re.compile(r"^\s*\.data\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+\.o)\s*$")
    placed = {}
    for line in MAP.read_text(errors="replace").splitlines():
        match = pattern.match(line)
        if match:
            obj = pathlib.Path(match.group(3)).name[:-2]
            if (DATA_OBJ / (obj + ".o")).exists():
                placed[obj] = (int(match.group(1), 16), int(match.group(2), 16))
    return placed


def directives(path):
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            parts = line.split(None, 1)
            yield parts[0], parts[1].strip() if len(parts) > 1 else ""


def step(offset, directive, args):
    if directive in (".space", ".zero"):
        return offset + int(args.split(",", 1)[0], 0)
    if directive in (".long", ".4byte", ".word"):
        return offset + 4 * max(1, len(args.split(",")))
    if directive in (".short", ".2byte"):
        return offset + 2 * max(1, len(args.split(",")))
    if directive == ".byte":
        return offset + len(args.split(","))
    if directive == ".p2align":
        align = 1 << int(args.split(",", 1)[0], 0)
        return (offset + align - 1) & ~(align - 1)
    return offset


def main():
    if not EXE.exists() or not MAP.exists():
        print("PS3 ELF or linker map is missing; run make -C port/ps3 first")
        return 1
    where, checksum = index()
    placed = placed_objects()
    records = []
    for name in sorted(where):
        if name not in placed:
            raise SystemExit(f"{name}: not present in {EXE.name}.map")
        source, source_offset = where[name]
        start, section_size = placed[name]
        path = DATA / (name + ".s")
        consumed = 0
        for directive, args in directives(path):
            consumed = step(consumed, directive, args)
        if consumed > section_size or section_size - consumed > 15:
            raise SystemExit(f"{name}: table is {consumed} bytes, linked section is {section_size}")
        offset = 0
        for directive, args in directives(path):
            if directive == ".space":
                length = int(args.split(",", 1)[0], 0)
                if length:
                    records.append((start + offset, source, source_offset + offset, length))
            offset = step(offset, directive, args)
    out = HAL / "EBOOT.BIN.dat"
    header = struct.pack("<4sIQ", b"BT3D", len(records), checksum)
    out.write_bytes(header + b"".join(struct.pack("<IIII", *record) for record in records))
    print(f"wrote {len(records)} little-endian data runs ({out.stat().st_size} bytes) to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
