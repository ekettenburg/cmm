# cmm installer (written in cmm)

`Install.cmm` is a self-installer for cmm, written in cmm itself. Given a JSON
manifest, it detects the current platform, downloads the cmm source and a zig
toolchain for that platform, unzips them, and builds the cmm compiler.

## What it does

1. Downloads a JSON manifest (URL is the first argument, or the built-in
   default).
2. Detects the platform with `Sys.platform()` — e.g. `linux-x86_64`,
   `macos-aarch64`, `windows-x86_64` (resolved at compile time).
3. Downloads `cmm.zip` and the `zig.zip` listed for this platform.
4. Unzips them into `./cmm` and `./zig` with the built-in `Zip.unzip`
   (handles DEFLATE; no external unzip tool needed).
5. Locates the `zig` binary (handling a versioned subdirectory) and uses it to
   compile the cmm compiler: `cmm/bootstrap/cmmc`.

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
cmmc build install/Install.cmm -o cmm-install
./cmm-install https://your.host/manifest.json
```

With no argument it uses the default manifest URL baked into `main()`; edit that
line (or always pass the URL) to point at your release.

## Notes

- HTTPS downloads are certificate-verified out of the box (vendored mbedTLS).
- Extraction uses the built-in `Zip.unzip` (DEFLATE-capable, in-process).
- It builds `cmmc` by invoking the downloaded `zig` as the C backend, matching
  how cmm uses zig everywhere else. If the source zip already ships
  `embedded_runtime.h`, the embed step is skipped; otherwise it is regenerated.
