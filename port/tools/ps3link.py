#!/usr/bin/env python3
"""Link the PS3 program and report unresolved symbols as temporary diagnostic placeholders."""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
OBJ = ROOT / "port/build/obj_ps3"
DATA_OBJ = ROOT / "port/build/obj_ps3_data"
HAL = ROOT / "port/build/ps3"
EXE = HAL / "bt3.elf"
STUBS = HAL / "stubs.c"
STUB_OBJ = HAL / "stubs.o"
MAP = HAL / "bt3.map"
LIBS = ["-Wl,--start-group", "-llv2", "-lio", "-lsysutil", "-lrt", "-lsysfs", "-lsysmodule", "-lrsx", "-lgcm_sys", "-laudio", "-lm", "-lpthread", "-Wl,--end-group"]


def objects(include_stubs=False):
    keep = {"ps3time.o", "ps3pad.o", "main_ps3.o", "plat_stub.o", "plat_libm.o", "ps3softint.o", "ps3video.o", "ps3memory.o", "ps3sync.o", "ps3compat.o", "ps3audio.o"}
    hal = [obj for obj in sorted(HAL.glob("*.o")) if obj.name in keep]
    extra = [STUB_OBJ] if include_stubs and STUB_OBJ.exists() else []
    return hal + sorted(DATA_OBJ.glob("*.o")) + sorted(OBJ.glob("*.o")) + extra


def symbol_sizes():
    out = {}
    paths = [ROOT / "config/symbol_addrs.txt", ROOT / "config/symbol_addrs_dbzp.txt"] + \
            sorted((ROOT / "config/symbols").glob("*.txt"))
    for path in paths:
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            match = re.match(r"(\w+)\s*=\s*0x[0-9A-Fa-f]+;\s*//\s*size:(0x[0-9A-Fa-f]+)", line)
            if match:
                out[match.group(1)] = int(match.group(2), 16)
    return out


def stub_fail(callsites):
    sizes = symbol_sizes()
    lines = ['/* Generated diagnostic placeholders; unresolved behavior remains. */',
             'extern int printf(const char *, ...);',
             'static int sStubsShown;',
             'static void stub_trace(const char *name) {',
             '    if (sStubsShown < 200) { printf("STUB %s\\n", name); sStubsShown++; }',
             '}',
             '#define STUB_FN(name) long name(void) { stub_trace(#name); return 0; }']
    funcs, data = [], []
    for name in callsites:
        if re.match(r"^(D_[0-9A-F]{8}|g[A-Za-z])", name):
            size = min(sizes.get(name) or (0x4000 if name.startswith("g") else 0x1000), 0x100000)
            data.append(f"char {name}[{size}];")
        elif re.match(r"^[A-Za-z_]", name):
            funcs.append(f"STUB_FN({name})")
    lines += funcs + ["/* data placeholders */"] + data
    STUBS.write_text("\n".join(lines) + "\n")
    print(f"  diagnostic placeholders: {len(funcs)} functions, {len(data)} zeroed objects")


def link(extra_args, include_stubs=False):
    cmd = (["ppu-gcc", "-w", "-Wl,--wrap=main", "-Wl,--no-warn-mismatch",
           "-Wl,--defsym=D_3BE71C=0x3BE71C",
           "-Wl,-Map=" + str(MAP), "-o", str(EXE)] + extra_args +
          [str(obj) for obj in objects(include_stubs)] +
          ["-L/usr/local/ps3dev/ppu/lib"] + LIBS)
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def missing(result):
    return sorted(set(re.findall(r"undefined reference to \x60([A-Za-z_][A-Za-z0-9_]*)'", result.stderr)))


def main():
    HAL.mkdir(parents=True, exist_ok=True)
    print("link layout: standard PSL1GHT")
    result = link(["-msoft-float"])
    count = 0
    for attempt in range(12):
        unresolved = missing(result)
        if not unresolved:
            break
        print(f"link pass {attempt + 1}: {len(unresolved)} unresolved symbols")
        stub_fail(unresolved)
        compiled = subprocess.run(["ppu-gcc", "-c", str(STUBS), "-o", str(STUB_OBJ), "-msoft-float"],
                                  cwd=ROOT, capture_output=True, text=True)
        if compiled.returncode:
            print("diagnostic placeholders did not compile:")
            print("\n".join(compiled.stderr.splitlines()[-12:]))
            return 1
        count += len(unresolved)
        result = link(["-msoft-float"], include_stubs=True)
    if result.returncode:
        print("LINK FAILED:")
        errors = [line for line in result.stderr.splitlines()
                  if "undefined reference" in line or "multiple definition" in line or "error:" in line]
        print("\n".join(errors[-24:]))
        print("--- linker stderr tail ---")
        print("\n".join(result.stderr.splitlines()[-60:]))
        return 1
    print("LINK OK:", EXE)
    if count:
        print(f"temporary diagnostic placeholders used: {count}; this ELF is not a runnable game build")
    subprocess.run(["ppu-readelf", "-lW", str(EXE)], check=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
