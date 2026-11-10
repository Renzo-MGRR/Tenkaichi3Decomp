#!/usr/bin/env python3
"""PS3 (ps3dev PPU + PSL1GHT) build of Tenkaichi3Decomp — workhorse experiment.

Compiles one game file through the PS3 pipeline and links its object:
  prepare (portsrc) -> preprocess (host gcc, exact flags of the PC build)
  -> eeconst.transform (PS2 float constants)
  -> ptr32.transform (4-byte pointers, libclang)
  -> clang -emit-llvm -target powerpc64-unknown-linux-gnu
     -> irfix.fix (memcpy/call-through-ptr32/datasection repairs)
     -> llc -mtriple=powerpc64-unknown-linux-gnu -filetype=obj
  -> ppu-gcc link (PSL1GHT)

Usage: python3 port/tools/ps3.py src/main.c ...
"""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "port/tools"))
import portsrc, eeconst, ptr32, irfix

OBJ = ROOT / "port/build/obj_ps3"
OUT = ROOT / "port/build/ps3"
# The target's own C headers (the newlib ppu-gcc links against): the port's files should be seen by the
# preprocessor exactly as the target will see them, not by the host's glibc.
LIBCINC = pathlib.Path("/usr/local/ps3dev/ppu/powerpc64-ps3-elf/include")
CPP = ["gcc", "-m32", "-std=gnu89", "-D_POSIX_THREADS=1", "-D_POSIX_MONOTONIC_CLOCK=1", "-E", "-O2", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-fno-strict-aliasing", "-ffp-contract=off", "-fcommon", "-w",
       "-fno-pic", "-fno-builtin", "-msoft-float", "-mno-sse", "-mno-mmx", "-malign-double", "-include", "port_libm.h",
       "-Iinclude", "-Iport/include", "-include", "port_compat.h"]
TARGET = ["--target=powerpc64-unknown-linux-gnu"]  # big-endian PPU

def compile_one(f, route="game", extra_cpp=(), hard_float=False):
    """route "game": the game's sources (compat header, port_libm forced in).
       route "port": the port's own sources (no compat header; PORT_PS3 defined; flags of undefined.py's
       port rules minus SF_FAST_CALLS, which stays off for PS3 v1: all soft float).
       hard_float: use the PPU's own floating-point unit. Only for code whose outside functions take
       integers (the renderer: Vertex fields are read from memory, so no float ABI crosses an object)."""
    if route == "port":
        # LP64, like the PPU target: with -m32 the target's <stdint.h> picks "unsigned long" for
        # uint32_t, and clang (powerpc64) then makes those fields 64 bits wide.
        cpp = ["gcc", "-m64", "-std=gnu99", "-D_POSIX_THREADS=1", "-D_POSIX_MONOTONIC_CLOCK=1", "-E", "-O2", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-fno-strict-aliasing", "-ffp-contract=off", "-fcommon", "-w",
               "-fno-pic", "-fno-builtin", "-msoft-float", "-mno-sse", "-mno-mmx",
               "-DPORT_PS3=1", "-include", "port_libm.h",
               "-Iinclude", "-Iport/include", "-Iport/src",
               "-isystem", LIBCINC]
    elif route == "newlib":
        # The PS2's maths library: no port_libm.h here (its #define pow Port_pow would rename the
        # library's own __DECL_SIMD_pow macro); newlib_shim.h comes in on top (see ps3port.py).
        cpp = ["gcc", "-m32", "-std=gnu99", "-D_POSIX_THREADS=1", "-D_POSIX_MONOTONIC_CLOCK=1", "-E", "-O2", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-fno-strict-aliasing", "-ffp-contract=off", "-fcommon", "-w",
               "-fno-pic", "-fno-builtin", "-msoft-float", "-mno-sse", "-mno-mmx", "-malign-double",
               "-Iport/include", "-isystem", LIBCINC]
    else:
        cpp = ["gcc", "-m32", "-std=gnu89", "-D_POSIX_THREADS=1", "-D_POSIX_MONOTONIC_CLOCK=1", "-DPORT_PS3=1", "-E", "-O2", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-fno-strict-aliasing", "-ffp-contract=off", "-fcommon", "-w",
               "-fno-pic", "-fno-builtin", "-msoft-float", "-mno-sse", "-mno-mmx", "-malign-double", "-include", "port_libm.h",
               "-Iinclude", "-Iport/include", "-include", "port_compat.h"]
    try:
        rel = f.relative_to(ROOT / "src")
        name = str(rel)[:-2].replace("/", "_")
    except ValueError:
        name = "p_" + str(f.relative_to(ROOT))[:-2].replace("/", "_")
    src, done, missing = portsrc.prepare(f)
    if missing:
        print("no C for:", f.name, " ".join(missing))
    o = OBJ / (name + ".o")
    i = pathlib.Path(str(o)[:-2] + ".i")
    r = subprocess.run(cpp + list(extra_cpp) + [f"-I{f.parent}", "-E", str(src)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        return None, f"cpp: {r.stderr[-400:]}"
    i.write_text(eeconst.transform(r.stdout))
    # ptr32: the game's pointers, 4 bytes
    std = "-std=gnu89"
    try:
        text = ptr32.transform(i, ["-x", "cpp-output", TARGET[0], std, "-fms-extensions", "-w",
                                   "-Wno-error=return-mismatch"])
    except RuntimeError as e:
        return None, f"ptr32: {e}"
    i.write_text(text, encoding="latin-1")
    # LLVM IR
    ll = pathlib.Path(str(o)[:-2] + ".ll")
    cmd = ["clang"] + TARGET + ["-S", "-emit-llvm", "-fms-extensions", "-w", "-Wno-error=return-mismatch",
                                "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int",
                                "-Wno-error=incompatible-function-pointer-types", "-Wno-error=incompatible-pointer-types",
                                "-x", "cpp-output", "-o", str(ll)]
    # strip -D flags: the input is preprocessed already
    c2 = [a for a in cmd]
    r = subprocess.run(c2 + [str(i)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        return None, "clang: " + (r.stderr or r.stdout)[-500:]
    text, _ = irfix.fix(ll.read_text())
    ll.write_text(text)
    # PPC64 BE object
    r = subprocess.run(["llc", "-O2", "-filetype=obj", "-relocation-model=static", "--mtriple=powerpc64-unknown-linux-gnu",
                        "-mattr=" + ("+hard-float" if hard_float else "-hard-float"), str(ll), "-o", str(o)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        return None, "llc: " + (r.stderr or r.stdout)[-600:]
    return o, None

def main(argv):
    OBJ.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    ok = 0
    for a in argv:
        f = ROOT / a
        o, err = compile_one(f)
        if err:
            print("FAILED", f.name, err[:400])
        else:
            print("ok", f.name)
            ok += 1
    return 0 if ok == len(argv) else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
