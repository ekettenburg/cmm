# C-- (cmm) Documentation

Start with the language reference, then dip into a library as needed.

## Language

- [Language reference](language.md) — syntax, types, operators, control flow,
  functions, class variables, magic constants, concurrency, memory model, and
  current limitations.

## Performance & size

Release builds (the default) are optimized for small, fast binaries:

- **Dead-code elimination** — compiled with `-ffunction-sections -fdata-sections`
  and linked with `--gc-sections`, so unused runtime natives (MySQL, regex, zip,
  …) are dropped from the executable.
- **Stripped symbols** — release binaries are stripped (`-s`); `--debug`/`-g`
  builds keep full symbols and are never stripped.
- **TLS only when used** — mbedTLS is linked only if the program actually calls
  `Http.*`. A program that never touches HTTPS pays nothing for TLS. Use
  `--tls` to force it on or `--no-tls` to force it off.

The effect is dramatic: a hello-world went from ~5 MB to ~12 KB; a real HTTPS
program is ~0.8 MB with only the TLS code it uses.

Memory management is a per-call **region allocator**: each function frame owns an
arena that is freed in one shot on return, with escaping return values copied to
the caller's arena. There is no GC or refcount overhead. (One caveat: allocations
made inside a single long-running frame — e.g. a `while true` server loop — are
not reclaimed until that frame returns.)

## Tooling

- **VS Code extension** — [`editor/vscode/`](../editor/vscode/): highlighting +
  live diagnostics via `cmmc check`.
- **`cmmc check <file>`** — parse + type-check only; reports **all** errors in one pass.
- **`--debug` / `-g`** — `-O0 -g`, `#line` source maps, `CMM_DEBUG` runtime.

## Built-in libraries

Always available, no import required.

| Library | Doc | Covers |
|---------|-----|--------|
| Strings | [builtin-strings.md](builtin-strings.md) | `String` methods + scalar conversions |
| Collections | [builtin-collections.md](builtin-collections.md) | `List`, `Dict`, literals |
| JSON / Data | [builtin-json.md](builtin-json.md) | `Json` namespace + the dynamic `Data` type |
| I/O | [builtin-io.md](builtin-io.md) | `Console`, `File` |
| Networking | [builtin-networking.md](builtin-networking.md) | `Http`, `Socket` |
| System | [builtin-system.md](builtin-system.md) | `Sys` (args/cwd/chdir/env/exec/…), `__FILE__`/`__DIR__` |
| AWS SigV4 | [lib-aws.md](lib-aws.md) | `use Aws;` — Signature Version 4 request signing |
| Amazon S3 | [lib-s3.md](lib-s3.md) | `use S3;` — get/put/delete/list + presigned URLs |
| AWS Lambda | [lib-lambda.md](lib-lambda.md) | run a compiled program as a Lambda `bootstrap` (Runtime API, CloudWatch, peak memory, deploy via SigV4) |
| Zip | [lib-zip.md](lib-zip.md) | build ZIP archives (preserves exec mode; used for Lambda packages) |
| Math / Date | [builtin-math-date.md](builtin-math-date.md) | `Math`, `Date` |
| Crypto / Base64 | [builtin-crypto.md](builtin-crypto.md) | `Crypto` (SHA-256/1, HMAC, random), `Base64` |
| MySQL | [lib-mysql.md](lib-mysql.md) | native `Mysql` client (native_password, async via run/wait) |
| Regex (preg) | [lib-preg.md](lib-preg.md) | `Preg` — PHP-style `preg_match`/`replace`/`split`/… |
| Concurrency | [builtin-concurrency.md](builtin-concurrency.md) | `run`, `wait`, `use`-locks |

## Standard library

Written in cmm, bundled with the compiler, imported with `use <Name>;`.

| Library | Doc | Covers |
|---------|-----|--------|
| `Str` | [stdlib-Str.md](stdlib-Str.md) | string helpers (repeat, join, pad, …) |
| `Arr` | [stdlib-Arr.md](stdlib-Arr.md) | list helpers (range, slice, unique, sum, …) |
| `Net` | [stdlib-Net.md](stdlib-Net.md) | transparent file-or-URL I/O |
| `Out` | [stdlib-Out.md](stdlib-Out.md) | output that accepts any value type |

## Deploying to AWS Lambda

- [build-amazon-linux.md](build-amazon-linux.md) — cross-compile a `bootstrap` for Amazon Linux 2023 from Windows or Linux (one zig backend)
- [lib-lambda.md](lib-lambda.md) — the Lambda runtime client + SigV4 deploy
- [lib-zip.md](lib-zip.md) — build the deployment .zip in cmm

## Examples

Runnable programs live in [`../examples`](../examples). Highlights: `Hello`,
`Bank` (locks), `Worker` (run/wait), `JsonDemo` (JSON round-trip), `Args`
(command-line arguments), `PathDemo` (`__FILE__`/`__DIR__`, cwd/chdir).
