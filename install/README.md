# cmm installer (written in cmm)

`Bootstrap.cmm` is a self-installer for cmm, written in cmm itself. Given a JSON
manifest, it detects the current platform, downloads the cmm source and a zig
toolchain for that platform, unzips them, and builds the cmm compiler.

> **Naming note:** keep the built exe's name (and this source file's name) free
> of words like `install`, `setup`, `update`, or `patch`. Windows' UAC
> "installer detection" heuristic auto-elevates any `.exe` whose *filename*
> matches those patterns, launching it in a separate elevated console before a
> single line of your code runs — which looks exactly like a silent crash (a
> flashing window, then nothing). That's why this file and its default output
> are named `Bootstrap`, not `Install`.

## What it does

1. Downloads a JSON manifest (URL is the first argument, or the built-in
   default).
2. Detects the platform with `Sys.platform()` — e.g. `linux-x86_64`,
   `macos-aarch64`, `windows-x86_64` (resolved at compile time).
3. Downloads `cmm.zip` and the `zig.zip` listed for this platform.
4. Unzips them into `./cmm` and `./zig` with the built-in `Zip.unzip`
   (handles DEFLATE; no external unzip tool needed).
5. Locates the `zig` binary (handling a versioned subdirectory) and uses it to
   compile the cmm compiler: `cmm/bootstrap/cmm`.

## Manifest format

```json
{
  "cmm": { "url": "https://.../cmm-src.zip" },
  "zig": {
    "linux-x86_64":   "https://.../zig-linux-x86_64.zip",
    "linux-aarch64":  "https://.../zig-linux-aarch64.zip",
    "macos-x86_64":   "https://.../zig-macos-x86_64.zip",
    "macos-aarch64":  "https://.../zig-macos-aarch64.zip",
    "windows-x86_64": "https://.../zig-windows-x86_64.zip"
  }
}
```

`cmm.zip` is expected to unpack to a tree containing `bootstrap/` and
`runtime/` (the standard source layout). See `manifest.json` for a template.

## Build and run

```sh
cmm build install/Bootstrap.cmm -o cmm-bootstrap
./cmm-bootstrap https://your.host/manifest.json
```

With no argument it uses the default manifest URL baked into `main()`; edit that
line (or always pass the URL) to point at your release.

## Notes

- HTTPS downloads are certificate-verified out of the box (vendored mbedTLS).
- Extraction uses the built-in `Zip.unzip` (DEFLATE-capable, in-process).
- It builds `cmm` by invoking the downloaded `zig` as the C backend, matching
  how cmm uses zig everywhere else. If the source zip already ships
  `embedded_runtime.h`, the embed step is skipped; otherwise it is regenerated.

## Cross-compiling the installer

The installer itself is just a cmm program, so it cross-compiles like any other:

```sh
cmm build install/Bootstrap.cmm -o cmm-bootstrap-x64.exe --target windows-x64
cmm build install/Bootstrap.cmm -o cmm-bootstrap         --target al2023     # Linux x64
```

`--target windows-x64` produces a self-contained PE32+ x86-64 `.exe` (needs zig:
`pip install ziglang`). The manifest platform keys must match `Sys.platform()`
exactly, e.g. `windows-x86_64`, `linux-x86_64`, `macos-aarch64`.

## PATH setup — `cmm` from anywhere

After building, the installer puts `cmm` on your PATH so it just
works, choosing the right method per system:

- **Linux / macOS** — installs launcher scripts into `/usr/local/bin` when it's
  writable, otherwise `~/.local/bin`, adding that dir to your shell profile
  (`~/.profile` plus `~/.zshrc`/`~/.bashrc`) if it isn't already on PATH. Open a
  new shell (or `source ~/.profile`) and run `cmm build app.cmm`.
- **Windows** — writes a `cmm.cmd` launcher and appends its directory to your
  **user** PATH via PowerShell. Open a new terminal and run `cmm build app.cmm`.

The launchers are thin wrappers that exec the real `cmm` by absolute path, so
the compiler still finds its `stdlib/` and `third_party/` tree correctly (a bare
symlink would break that on macOS). A `cmm` launcher is also dropped in the
install directory, so `./cmm` works immediately without opening a new shell.
