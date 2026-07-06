"""Command-line interface for the cmm compiler.

Usage:
    cmmc build <file.cmm> [-o OUT] [--emit-c] [--keep-c] [--tls|--no-tls]
                          [--no-verify] [--target NAME] [--lambda] [--static]
                          [--cc CMD] [-v]
    cmmc run   <file.cmm> [-v] [-- ARGS...]
    cmmc emit  <file.cmm>            # write the generated C and stop
    cmmc check <file.cmm>            # parse + type-check only; print diagnostics

    --debug / -g  on build/run: -O0 -g, #line source mapping, CMM_DEBUG runtime
    cmmc version

Cross-compile targets (--target):
    al2023         Amazon Linux 2023, x86_64 (glibc 2.34)   [Lambda default]
    al2023-arm64   Amazon Linux 2023, arm64/Graviton
    al2023-static  fully static (musl) — runs on any Linux
  --lambda is shorthand for --target al2023. These use zig as the C backend
  (install once with:  pip install ziglang); works on Windows and Linux.
"""

import os
import subprocess
import sys

from .lexer import CmmError
from . import compiler
from . import __version__


def _print_cmm_error(e):
    loc = e.file or "<input>"
    sys.stderr.write(f"{loc}:{e.line}:{e.col}: error: {e.message}\n")


def cmd_build(args):
    out = None
    emit_c = keep_c = verbose = debug = False
    tls = "auto"
    target = target_os = None
    static = False
    no_verify = False
    files = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-o", "--out"):
            i += 1
            out = args[i]
        elif a == "--emit-c":
            emit_c = True
        elif a == "--keep-c":
            keep_c = True
        elif a == "--no-tls":
            tls = "off"
        elif a == "--tls":
            tls = "on"
        elif a == "--no-verify":
            no_verify = True
        elif a == "--cc":
            i += 1
            os.environ["CMMC_CC"] = args[i]
        elif a == "--static":
            static = True
        elif a == "--target":
            i += 1
            target = args[i]
        elif a == "--target-os":
            i += 1
            target_os = args[i]
        elif a == "--lambda":
            target = "al2023"
        elif a in ("-v", "--verbose"):
            verbose = True
        elif a in ("--debug", "-g"):
            debug = True
        else:
            files.append(a)
        i += 1
    if len(files) != 1:
        sys.stderr.write("cmmc build: expected exactly one .cmm file\n")
        return 2
    try:
        result = compiler.build(files[0], out_path=out, emit_c=emit_c,
                                 keep_c=keep_c, verbose=verbose, tls=tls,
                                 target=target, static=static,
                                 target_os=target_os, debug=debug,
                                 no_verify=no_verify)
    except CmmError as e:
        _print_cmm_error(e)
        return 1
    except compiler.CompileError as e:
        sys.stderr.write(f"error: {e}\n")
        return 1
    if emit_c:
        print(result)
        return 0
    exe, remote = result
    print(f"built {exe}")
    if remote:
        sys.stderr.write(
            "note: remote `use \"<url>\";` imports are not fetched in this "
            "build; referenced classes must be available locally:\n")
        for u in remote:
            sys.stderr.write(f"      {u}\n")
    return 0


def cmd_run(args):
    verbose = False
    debug = False
    passthru = []
    files = []
    if "--" in args:
        idx = args.index("--")
        passthru = args[idx + 1:]
        args = args[:idx]
    for a in args:
        if a in ("-v", "--verbose"):
            verbose = True
        elif a in ("--debug", "-g"):
            debug = True
        else:
            files.append(a)
    if len(files) != 1:
        sys.stderr.write("cmmc run: expected exactly one .cmm file\n")
        return 2
    try:
        exe, remote = compiler.build(files[0], verbose=verbose, debug=debug)
    except CmmError as e:
        _print_cmm_error(e)
        return 1
    except compiler.CompileError as e:
        sys.stderr.write(f"error: {e}\n")
        return 1
    proc = subprocess.run([exe] + passthru)
    return proc.returncode


def cmd_emit(args):
    return cmd_build(args + ["--emit-c"])


def cmd_check(args):
    files = [a for a in args if not a.startswith("-")]
    if len(files) != 1:
        sys.stderr.write("cmmc check: expected exactly one .cmm file\n")
        return 2
    try:
        errors = compiler.check(files[0])
    except compiler.CompileError as e:
        sys.stderr.write(f"error: {e}\n")
        return 1
    for e in errors:
        _print_cmm_error(e)
    return 1 if errors else 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        sys.stderr.write(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    if cmd == "build":
        return cmd_build(rest)
    if cmd == "run":
        return cmd_run(rest)
    if cmd == "emit":
        return cmd_emit(rest)
    if cmd == "check":
        return cmd_check(rest)
    if cmd in ("version", "--version", "-V"):
        print(f"cmmc {__version__}")
        return 0
    if cmd in ("help", "--help", "-h"):
        sys.stdout.write(__doc__)
        return 0
    sys.stderr.write(f"cmmc: unknown command '{cmd}'\n")
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
