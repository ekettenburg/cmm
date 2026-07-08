# Packaging C-- as a portable Windows toolchain

Goal: a single folder you can copy to any Windows machine — no installer, no
admin, no "install a C compiler first" — where `cmm.exe` finds a **bundled**
C compiler sitting next to it.

```
cmm-win64\
  cmm.exe          the C-- compiler (the C-- runtime is embedded inside it)
  bin\             the bundled C backend: gcc.exe (or tcc.exe) + its support files
  lib\             extra import libraries (e.g. OpenSSL) — optional, for TLS
  include\         extra headers (e.g. openssl\) — optional, for TLS
  examples\        sample .cmm programs
  README.txt
```

`cmm.exe` resolves its C backend in this order:

1. `--cc <path>` on the command line,
2. the `LANGC_CC` environment variable,
3. a **bundled** compiler at `<folder>\bin\{gcc,clang,cc,tcc}.exe` — and when one
   is found it also adds `<folder>\include` and `<folder>\lib` to the compile,
4. a system `clang`/`gcc`/`cl` on `PATH`.

So if `bin\gcc.exe` exists next to `cmm.exe`, it is used automatically and the
package is fully self-contained. Verify with `cmm build foo.cmm -v` — the
first line prints e.g. `cc: C:\...\bin\gcc.exe  [gcc-like, bundled]`.

`cmm.exe` itself only links `KERNEL32.dll` and `msvcrt.dll`, which ship with
Windows, so the compiler binary needs nothing else.

---

## Option A — MinGW-w64 (recommended: full optimization, threads, easy TLS)

This is a real GCC. It is relocatable: the toolchain finds its own CRT, libgcc
and libwinpthread relative to the `gcc.exe` location, so it works from any path.

1. Download a portable **WinLibs** UCRT build (https://winlibs.com) — the
   "GCC ... UCRT runtime" Zip. (LLVM/Clang variants also work.)
2. Unzip it; you get a `mingw64\` folder containing `bin\`, `lib\`, `include\`,
   and `x86_64-w64-mingw32\`.
3. Make `mingw64\` *be* the package: drop `cmm.exe` and `examples\` into the
   top of it and rename the folder to `cmm-win64\`. Now:
   - `cmm-win64\cmm.exe`  ← the compiler
   - `cmm-win64\bin\gcc.exe` ← found automatically (step 3 above)
   - `gcc.exe` locates its own sysroot under `cmm-win64\` (so the whole GCC
     tree must stay intact — don't copy only `bin\`).

That's it. `cmm-win64\cmm.exe build examples\Demo.cmm -o demo.exe` works on a
machine with no compiler installed.

### TLS on Windows (optional)
`https://` needs OpenSSL at build time. With MinGW the easiest source is MSYS2's
`mingw-w64-x86_64-openssl`, or any prebuilt OpenSSL-for-MinGW:
- copy `libssl.dll.a` and `libcrypto.dll.a` into `cmm-win64\lib\`,
- copy the `openssl\` headers into `cmm-win64\include\`,
- copy `libssl-3-x64.dll` and `libcrypto-3-x64.dll` next to your built `.exe`
  (or into `cmm-win64\bin\`, which is on the program's search path if the
  package dir is on PATH).

`cmm` auto-detects this (it probe-compiles against `include\`/`lib\`); force it
with `--tls`, or skip with `--no-tls`.

---

## Option B — TinyCC (smallest: ~1–3 MB, single tiny compiler)

TinyCC compiles and links natively on Windows. No optimizer, but it builds the
straightforward C99 that C-- emits and links Winsock/threads fine.

1. Download the Windows **tcc** build (`tcc-0.9.27-win64-bin.zip` or newer).
2. Put its contents inside `cmm-win64\bin\` so you have:
   - `cmm-win64\bin\tcc.exe`
   - `cmm-win64\bin\include\`  (tcc's headers — tcc finds these relative to itself)
   - `cmm-win64\bin\lib\`      (`libtcc1.a`, the Win32 `*.def` import libs)
3. `cmm` finds `bin\tcc.exe` and uses the `tcc` codepath automatically.

TinyCC + OpenSSL is fiddly (you need OpenSSL import defs tcc understands); if you
want TLS, prefer Option A. Build with `--no-tls` when using tcc without OpenSSL.

---

## Building `cmm.exe`

On Windows: open the WinLibs/MinGW shell (or any prompt with `gcc` on PATH) and

```
cd bootstrap
gcc -std=c99 -O2 embed.c -o embed_tool.exe
embed_tool.exe ..\runtime\cmm_runtime.h ..\runtime\cmm_runtime.c embedded_runtime.h
gcc -std=c99 -O2 cmm.c -o cmm.exe
```

(`build_cmm.bat` does exactly this.) Cross-building from Linux/macOS with
MinGW works too:

```
x86_64-w64-mingw32-gcc -std=c99 -O2 cmm.c -o cmm.exe
```

`cmm.c` already `#include`s `embedded_runtime.h`, so regenerate that first if
the runtime changed.

---

## Quick self-test on the target machine

```
cmm-win64\cmm.exe version
cmm-win64\cmm.exe build examples\Demo.cmm -o demo.exe -v   ::  prints the bundled cc
demo.exe
```

If `-v` shows `[..., bundled]` and `demo.exe` runs, the package is portable and
complete.
