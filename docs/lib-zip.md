# Zip

Build ZIP archives from in-memory data. `Zip` is built in; no import is
required. It uses the STORE method (no compression dependency) and writes Unix
permissions into the archive, so an extracted file keeps its executable bit —
which is exactly what an AWS Lambda `bootstrap` needs.

## API

| Call | Signature | Description |
|------|-----------|-------------|
| `Zip.build` | `(entries: List[Data]) -> String` | build a zip archive and return its bytes |
| `Zip.unzip` | `(zipPath: String, destDir: String) -> Int` | extract a `.zip` file into `destDir`; returns the number of files written, or `-1` on error |

Each entry is a dict with:

| Field | Type | Description |
|-------|------|-------------|
| `name` | String | path of the entry inside the archive |
| `content` | String | file bytes (cmm strings are binary-safe) |
| `mode` | Int (optional) | Unix permission bits, e.g. `493` for `rwxr-xr-x` |
| `exec` | Bool (optional) | shortcut: `true` → mode `0755`, otherwise `0644` |

The returned value is the raw archive — write it with `File.write`, or hand it
straight to `Lambda.create` / `Lambda.updateCode`.

```
entries = [];
boot = {"name": "bootstrap", "content": File.read("bootstrap"), "exec": true};
entries.add(boot);
note = {"name": "README.txt", "content": "deployed by cmm"};
entries.add(note);

pkg = Zip.build(entries);
ok = File.write("function.zip", pkg);
```

## Notes

- Entries are stored uncompressed (STORE). The archive is valid everywhere and
  CRC-checked; it is not size-optimised (DEFLATE compression is not implemented
  yet).
- `content` may contain arbitrary bytes including NULs — `File.read` returns
  binary content faithfully, so zipping a compiled binary works directly.
- Reading/extracting archives is not provided yet; `Zip.build` is write-only.

## Extracting

`Zip.unzip(path, dest)` reads a `.zip` from disk and writes its contents under
`dest`, creating subdirectories as needed. It handles both stored and
DEFLATE-compressed entries (a small public-domain inflater is built into the
runtime, so there is no external dependency). On Unix it preserves a file's
executable bit when the archive records Unix permissions. It returns the number
of files extracted, or `-1` if the archive can't be read.

```
n = Zip.unzip("toolchain.zip", "vendor/zig");
if (n < 0) {
    Console.println("extract failed");
}
```
