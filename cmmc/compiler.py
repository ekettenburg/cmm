"""Compiler driver for cmm.

Resolves `use` imports, runs lexer -> parser -> analyzer -> codegen for the
whole program, then invokes a C compiler to produce a native executable.

The runtime (cmm_runtime.c / cmm_runtime.h) lives next to this package under
../runtime and is compiled together with the generated C file.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

from .lexer import tokenize, CmmError
from .parser import parse
from .analyzer import Analyzer
from . import codegen
from . import types as T


HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME_DIR = os.path.normpath(os.path.join(HERE, "..", "runtime"))


class CompileError(Exception):
    pass


# --------------------------------------------------------------------------
# Import resolution
# --------------------------------------------------------------------------
def _class_name_from_path(path):
    return os.path.splitext(os.path.basename(path))[0]


def load_program(entry_path):
    """Parse the entry file and transitively every locally `use`d class.

    Remote (`use "http..."`) imports are recorded but not fetched in this
    build; they are reported as a limitation if encountered.
    Returns (program_dict, entry_classname, remote_uses).
    """
    entry_path = os.path.abspath(entry_path)
    search_dirs = [os.path.dirname(entry_path)]
    for d in os.environ.get("CMMC_PATH", "").split(os.pathsep):
        if d:
            search_dirs.append(d)
    stdlib = os.path.normpath(os.path.join(HERE, "..", "stdlib"))
    if os.path.isdir(stdlib):
        search_dirs.append(stdlib)

    program = {}        # classname -> Module
    remote = []
    pending = [entry_path]
    seen_files = set()

    while pending:
        path = pending.pop()
        if path in seen_files:
            continue
        seen_files.add(path)
        if not os.path.exists(path):
            raise CompileError(f"cannot find source file: {path}")
        with open(path, "r", encoding="utf-8") as fh:
            src = fh.read()
        module = parse(src, path)

        expected = _class_name_from_path(path)
        if module.classname != expected:
            raise CompileError(
                f"{path}: class '{module.classname}' must live in a file "
                f"named '{module.classname}.cmm'")
        program[module.classname] = module

        for u in module.uses:
            if u.url:
                remote.append(u.url)
                continue
            name = u.name
            if name in T.NATIVE_CLASSES:
                continue            # provided by the runtime
            if name in program:
                continue
            found = None
            for d in search_dirs:
                cand = os.path.join(d, name + ".cmm")
                if os.path.exists(cand):
                    found = cand
                    break
            if not found:
                raise CompileError(
                    f"{path}: cannot resolve `use {name};` — no {name}.cmm "
                    f"found next to the entry file")
            pending.append(found)

    entry_class = _class_name_from_path(entry_path)
    return program, entry_class, remote


# --------------------------------------------------------------------------
# C compiler discovery
# --------------------------------------------------------------------------
def _which(prog):
    from shutil import which
    return which(prog)


def find_zig():
    """Locate a zig toolchain. Returns the command as a list, or None."""
    z = os.environ.get("CMMC_ZIG")
    if z:
        return z.split()
    if _which("zig"):
        return ["zig"]
    for py in ("python3", "python"):
        if _which(py):
            try:
                if subprocess.run([py, "-m", "ziglang", "version"],
                                  capture_output=True).returncode == 0:
                    return [py, "-m", "ziglang"]
            except OSError:
                pass
    return None


def find_c_compiler():
    """Return (cc_list, kind). One toolchain everywhere: prefer zig when present."""
    ov = os.environ.get("CMMC_CC")
    if ov:
        toks = ov.split()
        base = os.path.basename(toks[0])
        return toks, ("msvc" if base in ("cl", "cl.exe") else "gcc-like")
    z = find_zig()
    if z:
        return z + ["cc"], "gcc-like"
    for cc in ("clang", "gcc", "cc"):
        if _which(cc):
            return [cc], "gcc-like"
    if _which("cl"):
        return ["cl"], "msvc"
    return None, None


# Cross-compile presets: name -> (zig target triple, static?)
_TARGETS = {
    "al2023":        ("x86_64-linux-gnu.2.34",  False),
    "lambda":        ("x86_64-linux-gnu.2.34",  False),
    "amazonlinux":   ("x86_64-linux-gnu.2.34",  False),
    "al2023-arm64":  ("aarch64-linux-gnu.2.34", False),
    "al2023-static": ("x86_64-linux-musl",      True),
}

def setup_target(target):
    """Resolve a named cross target. Returns (cc_list, kind, target_os, static)."""
    if target not in _TARGETS:
        raise CompileError("unknown --target '%s' (use: %s)"
                           % (target, ", ".join(_TARGETS)))
    triple, static = _TARGETS[target]
    ov = os.environ.get("CMMC_CC")
    if ov:
        return ov.split(), "gcc-like", "linux", static
    z = find_zig()
    if not z:
        raise CompileError(
            "building for Amazon Linux needs zig (one toolchain for all hosts).\n"
            "  install once:  pip install ziglang     (Windows and Linux)\n"
            "  or put zig from ziglang.org on PATH, or set CMMC_ZIG=<path>.")
    return z + ["cc", "-target", triple], "gcc-like", "linux", static


THIRD_PARTY = os.path.normpath(os.path.join(RUNTIME_DIR, "..", "third_party"))

def find_tls_dir():
    """Locate the vendored mbedTLS tree (third_party/). Returns path or None."""
    env = os.environ.get("CMMC_TLS_DIR")
    cands = [env] if env else []
    cands += [THIRD_PARTY]
    for d in cands:
        if d and os.path.exists(os.path.join(d, "mbedtls", "include", "mbedtls",
                                             "ssl.h")) \
             and os.path.exists(os.path.join(d, "cmm_ca_certs.h")):
            return d
    return None


def _cache_dir(key):
    root = os.environ.get("CMMC_CACHE")
    if not root:
        if sys.platform == "win32":
            root = os.path.join(os.environ.get("LOCALAPPDATA", "."), "cmm", "cache")
        else:
            root = os.path.join(os.path.expanduser("~"), ".cache", "cmm")
    d = os.path.join(root, "tls", key)
    os.makedirs(d, exist_ok=True)
    return d


def ensure_tls_lib(cc, target_key, verbose=False):
    """Build (once) and cache the combined mbedTLS object for this target.
    Returns the object path, or None if TLS can't be provided."""
    tdir = find_tls_dir()
    if not tdir:
        return None
    obj = os.path.join(_cache_dir(target_key), "cmmtls_min.o")
    if os.path.exists(obj):
        return obj
    inc = os.path.join(tdir, "mbedtls", "include")
    libdir = os.path.join(tdir, "mbedtls", "library")
    manifest = os.path.join(tdir, "mbedtls", "sources.list")
    with open(manifest) as fh:
        srcs = [os.path.join(libdir, ln.strip())
                for ln in fh if ln.strip()]
    if verbose:
        sys.stderr.write("  building TLS support for '%s' (one-time, ~15s)...\n"
                         % target_key)
    # one invocation: partial-link all sources into a single relocatable object
    # minimal client profile: -I{tdir} finds cmm_mbedtls_config.h
    cmd = [*cc, "-w", "-O2", "-ffunction-sections", "-fdata-sections",
           f"-I{inc}", f"-I{tdir}",
           "-DMBEDTLS_CONFIG_FILE=<cmm_mbedtls_config.h>",
           "-r", *srcs, "-o", obj]
    if subprocess.run(cmd, capture_output=True, text=True).returncode != 0:
        if os.path.exists(obj):
            os.remove(obj)
        return None
    return obj


def compile_to_executable(c_path, out_path, verbose=False, tls="auto",
                          target=None, static=False, target_os=None, debug=False,
                          uses_http=True, no_verify=False):
    if target:
        cc, kind, target_os, static = setup_target(target)
    else:
        cc, kind = find_c_compiler()
    if cc is None:
        raise CompileError(
            "no C compiler found. Easiest fix: pip install ziglang "
            "(one toolchain for Windows and Linux). Or install clang/gcc/MSVC.")
    runtime_c = os.path.join(RUNTIME_DIR, "cmm_runtime.c")

    tls_defs, tls_link = ([], [])
    # auto: only pay for TLS if the program actually calls Http.*
    want_tls = (tls == "on") or (tls == "auto" and uses_http)
    if want_tls and kind == "gcc-like":
        key = target if target else ("host-static" if static else "host")
        obj = ensure_tls_lib(cc, key, verbose=verbose)
        if obj:
            tdir = find_tls_dir()
            tls_defs = ["-DCMM_HAVE_TLS",
                        f"-I{os.path.join(tdir, 'mbedtls', 'include')}",
                        f"-I{tdir}",
                        "-DMBEDTLS_CONFIG_FILE=<cmm_mbedtls_config.h>"]
            if no_verify:
                # drop the embedded CA bundle and disable cert verification
                tls_defs.append("-DCMM_TLS_NO_VERIFY")
            tls_link = [obj]
        elif tls == "on":
            raise CompileError(
                "TLS was requested (--tls) but the bundled TLS library was not "
                "found. cmm ships TLS as a vendored mbedTLS tree (third_party/) "
                "compiled by zig; ensure it's present (or set CMMC_TLS_DIR) and "
                "that zig is installed (pip install ziglang).")

    tgt_win = (target_os == "windows") if target_os else (sys.platform == "win32")
    opt = (["-O0", "-g", "-DCMM_DEBUG"] if debug
           else ["-O2", "-ffunction-sections", "-fdata-sections"])
    if kind == "gcc-like":
        cmd = [*cc, "-std=c99", *opt]
        if static:
            cmd.append("-static")
        cmd += [f"-I{RUNTIME_DIR}", *tls_defs, c_path, runtime_c,
                "-o", out_path, "-lm"]
        if tgt_win:
            cmd += ["-lws2_32", "-ladvapi32"]
        else:
            cmd.append("-lpthread")
        cmd.extend(tls_link)
        if tgt_win and tls_link:
            cmd.append("-lbcrypt")
        if not debug:
            # drop unreferenced runtime/TLS code and strip symbols
            cmd += ["-Wl,--gc-sections", "-s"]
    else:  # msvc
        mopt = (["/Od", "/Zi", "/DCMM_DEBUG"] if debug
                else ["/O2", "/Gy", "/Gw"])
        cmd = [*cc, "/nologo", *mopt, f"/I{RUNTIME_DIR}", *tls_defs,
               c_path, runtime_c,
               "ws2_32.lib", "advapi32.lib", *tls_link, f"/Fe:{out_path}"]
        if not debug:
            cmd += ["/link", "/OPT:REF", "/OPT:ICF"]

    if verbose or debug:
        print("  " + " ".join(cmd))
        print("  TLS: " + ("enabled (mbedTLS, cert-verified)" if tls_defs
                           else "disabled (https:// will return empty)"))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise CompileError(
            "C compiler failed:\n" + (proc.stdout or "") + (proc.stderr or ""))
    return out_path


# --------------------------------------------------------------------------
# Top-level build
# --------------------------------------------------------------------------

def check(entry_path):
    """Front-end only: load + analyze in lint mode. Returns a list of all
    CmmError diagnostics found (empty if the file is clean)."""
    try:
        program, entry, _remote = load_program(entry_path)
    except CmmError as e:
        return [e]
    an = Analyzer(program, entry)
    an.collect = True
    try:
        an.analyze()
    except CmmError as e:
        an.errors.append(e)
    return an.errors
def build(entry_path, out_path=None, emit_c=False, keep_c=False, verbose=False,
          tls="auto", target=None, static=False, target_os=None, debug=False,
          no_verify=False):
    program, entry, remote = load_program(entry_path)

    analyzer = Analyzer(program, entry)
    analyzer.analyze()

    c_src = codegen.generate(program, entry, debug=debug)
    if debug:
        keep_c = True
    uses_http = "cmm_http_" in c_src

    base = os.path.splitext(os.path.abspath(entry_path))[0]
    c_path = base + ".c"
    with open(c_path, "w", encoding="utf-8") as fh:
        fh.write(c_src)

    if out_path is None:
        eff_os = target_os or ("linux" if target else None)
        if eff_os:
            out_path = base + (".exe" if eff_os == "windows" else "")
        else:
            out_path = base + (".exe" if sys.platform == "win32" else "")

    if emit_c:
        if verbose:
            print(f"  wrote {c_path}")
        return c_path

    compile_to_executable(c_path, out_path, verbose=verbose, tls=tls,
                          target=target, static=static, target_os=target_os,
                          debug=debug, uses_http=uses_http, no_verify=no_verify)
    if not keep_c:
        try:
            os.remove(c_path)
        except OSError:
            pass

    return out_path, remote
