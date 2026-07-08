# C-- (cmm)

A small, statically-checked programming language that compiles to native
executables. It lowers `.cmm` source to portable C99 and compiles it with
**zig** — one toolchain that cross-compiles to every target from any host.
**Windows is a first-class target** — threading and sockets are abstracted over
Win32/Winsock and POSIX.

```
Main.cmm  ──►  cmm (lex ► parse ► analyze ► codegen)  ──►  Main.c
Main.c + cmm_runtime.c  ──►  zig cc  ──►  native executable
```

There are **two interchangeable implementations of the same compiler**:

* **`bootstrap/cmm`** — a single native binary written in C. It has **no
  Python dependency**: the only thing it needs is the C compiler the toolchain
  already uses to link programs. The C-- runtime is embedded inside it, so the
  binary is fully self-contained (≈150 KB, libc-only). This is the recommended
  way to distribute the toolchain.
* **`cmmc/` (Python)** — the original reference implementation. It is kept as
  an oracle; for every example it emits **byte-for-byte identical** C to the
  native compiler.

## Quick start (native compiler, no Python)

Build the compiler once (needs only a C compiler), then use it:

```sh
cd bootstrap && ./build_cmm.sh          # produces ./cmm    (Windows: build_cmm.bat)

./cmm run   examples/Hello.cmm
./cmm build examples/Demo.cmm -o demo
```

`bootstrap/cmm` is a drop-in replacement for `python3 -m cmmc`: same
`build` / `run` / `emit` / `version` commands and the same flags.

## Portable Windows package

The `cmm-win64` package runs on any Windows box with **no Python and no admin**,
but — because cmm compiles through C — it does need a **C compiler** available.
You can either bundle one next to `cmm.exe` or put one on `PATH`:

```
cmm-win64\
  cmm.exe        compiler (C-- runtime embedded)
  bin\           optional: drop gcc.exe (+DLLs) or tcc.exe here to bundle a backend
  third_party\     vendored mbedTLS (out-of-the-box HTTPS, compiled by zig)
  examples\  stdlib\
  src\           full source + build_cmm.bat to rebuild cmm.exe on Windows
  README.txt     setup steps
```

Backend resolution: `--cc <path>` → `$CMMC_CC` → **zig** (`zig` on `PATH`,
`$CMMC_ZIG`, or the `ziglang` pip package). Confirm with `cmm build foo.cmm -v`
— it prints the exact `zig cc` command. Install zig with `pip install ziglang`
(Windows, macOS, Linux) or grab a build from ziglang.org.

`bash bootstrap/make_win_package.sh` stages `dist/cmm-win64/` (cross-builds
`cmm.exe` with `zig cc -target x86_64-windows-gnu`) and includes the `src\`
build kit. More detail on the bundled TLS is in
**`bootstrap/PACKAGING-windows.md`**.

## Quick start (Python reference)

Needs **Python 3.8+** and a **C compiler** on your PATH.

```sh
# Unix / macOS
python3 -m cmmc run examples/Hello.cmm
python3 -m cmmc build examples/Demo.cmm -o demo

# Windows (Developer Command Prompt or MinGW shell)
python -m cmmc run examples\Hello.cmm
build.bat run examples\Hello.cmm
```

CLI (same flags on both implementations — `cmm` natively, `python -m cmmc` for
the Python reference):

```
cmm build <file.cmm> [-o OUT] [--emit-c] [--keep-c] [--tls|--no-tls] [-v]
cmm run   <file.cmm> [-v] [-- ARGS...]
cmm emit  <file.cmm>          # write generated C and stop
cmm version
```

The entry file's class is the program entry point; its `main()` method runs at
startup. `use OtherClass;` pulls in `OtherClass.cmm` from the same directory.

## Editor support & debugging

A **VS Code extension** lives in [`editor/vscode/`](editor/vscode/): syntax
highlighting plus live diagnostics that run `cmm check` and surface compiler
errors in the Problems panel as you type. Copy it into
`~/.vscode/extensions/` (see its README) or package it with `vsce`.

`cmm check <file.cmm>` type-checks a file and prints `file:line:col: error: …`
diagnostics without building; reports every error it finds in one pass — handy for editors and CI.

Build or run with **`--debug` / `-g`** for great debug output: `-O0`, DWARF
symbols, and `#line` directives that map the generated C back to your `.cmm`
source so `gdb`/`lldb` step through the original file. Debug builds also print
the C-compiler command and keep the generated `.c`, and the runtime prints a
startup banner and a crash handler.

## Documentation

Full docs live in [`docs/`](docs/README.md): a [language reference](docs/language.md)
plus one page per library (strings, collections, JSON/Data, I/O, networking,
system, math/date, concurrency, and each `use`-able standard library module).

**AWS & data (0.8.0):** built-in [`Crypto`/`Base64`](docs/builtin-crypto.md)
(SHA-256/1, HMAC, CSPRNG), a native [`Mysql`](docs/lib-mysql.md) client
(`mysql_native_password`, async via `run`/`wait`), PHP-style
[`Date.date`/`gmdate`](docs/builtin-math-date.md) formatting, a PHP-style
[`Preg`](docs/lib-preg.md) regex engine, and the
[`Aws`](docs/lib-aws.md) (SigV4) + [`S3`](docs/lib-s3.md) libraries — SigV4 and
S3 signing are verified against AWS's own test vectors.

**TLS:** HTTPS runs on a trimmed **TLS 1.2 client** profile of vendored mbedTLS
(ECDHE + AES-GCM with AES-NI / ARMv8 hardware acceleration, full X.509
verification against a DER-packed Mozilla root set; no server, 1.3/PSA, or DTLS
code). A cert-verified HTTPS binary is ~350 KB; TLS is linked only when a
program calls `Http.*`, so a Lambda handler with no outbound HTTPS is ~17 KB.
Build `--no-verify` to omit the CA roots and skip verification for the smallest
binary (~185 KB). See [docs/builtin-networking.md](docs/builtin-networking.md).

## Build for AWS Lambda (Amazon Linux 2023)

One backend, every host. Install zig once (`pip install ziglang`, same on
Windows and Linux), then cross-compile a Lambda `bootstrap`:

```
cmm build handler.cmm -o bootstrap --target al2023
```

It produces an x86-64 ELF pinned to glibc 2.34 (AL2023's version) that runs on
`provided.al2023` unchanged; `--target al2023-arm64` and `--target al2023-static`
are also available. If zig is installed it becomes cmm's normal host compiler
too, so there's just one toolchain. Full guide:
[docs/build-amazon-linux.md](docs/build-amazon-linux.md).

## Language at a glance

One class per file; the filename must match the class name.

```
class Counter;

@count: Int;                 // class variable (instance state, lockable)

fn increment() {
    @count++;
}

fn value() -> Int {
    n = @count;              // local; type inferred at first assignment
    return n;                // return exactly one variable
}

fn main() {
    increment();
    increment();
    Console.println(value());
}
```

Highlights, all implemented and exercised by the examples:

- **No null.** Every type has an automatic empty value (`Int`→0, `String`→"",
  `List`→[], `Dict`→{}, user class→empty instance). Test with `empty(x)`.
- **Two variable kinds.** Class variables are `@`-prefixed and hold the only
  shared mutable state; locals are type-inferred and fixed at first assignment.
- **One operator per expression.** Compounds must be parenthesized:
  `a + (b * c)` is valid, `a + b * c` is a compile error. No precedence rules.
- **Imports are whole classes.** `use Date;` (local) — no aliases, no wildcards.
- **Control flow & data.** `if/else`, `while`, `for x in items`, list literals
  `[1, 2, 3]` with `.add/.remove/.length`, dict literals `{"a": 1}` (a missing
  key returns the empty value), and a dynamic `Data` type for JSON.
- **Concurrency.** `job = run slow(x);` runs the call on an OS thread and returns
  a `Job[T]`; `result = wait job;` joins and yields the value.
- **Locks.** `use @balance { ... }` acquires the class variable's mutex;
  `use @a, @b { ... }` locks in a deterministic order to avoid deadlock.
- **Memory.** No GC and no manual frees. Each function call runs in its own
  region (arena); when the call returns, its region is freed and only the
  returned value is relocated into the caller — so transient allocations are
  reclaimed continuously during execution, not just at exit. Longer-lived
  state reachable from `@`class variables is shared, not copied. Invisible to
  the programmer.

Native classes available: `Console`, `String`, `Int`, `Float`, `Bool`, `List`,
`Dict`, `Data`, `Json`, `File`, `Math`, `Date`, `Socket`, `Http`, `Job`.

## Examples

| File | Shows |
|------|-------|
| `examples/Hello.cmm`    | minimal program |
| `examples/Counter.cmm`  | class variables, methods, return |
| `examples/Demo.cmm`     | lists, dicts, loops, if/else, imports, string methods |
| `examples/Greeter.cmm`  | a second class imported by `Demo` |
| `examples/Worker.cmm`   | `run` / `wait` concurrency |
| `examples/Bank.cmm`     | `use` locks under thread contention (deterministic result) |
| `examples/DataDemo.cmm` | `empty()`, `Json.decode`/`encode`, missing-key empties |
| `examples/MemReclaim.cmm` | per-function memory reclaim (allocates ~13 GB cumulatively, peaks ~11 MB) |
| `examples/LockReturn.cmm` | `return` inside a `use` lock auto-unlocks across threads |
| `examples/HttpsDemo.cmm` | `Http.get` over verified HTTPS/TLS (bundled mbedTLS) |
| `examples/StdDemo.cmm`  | the `Str` / `Arr` / `Net` / `Out` standard library |
| `examples/JsonDemo.cmm` | JSON decode → navigate/mutate the flexible `Data` → encode + pretty |
| `examples/Args.cmm`     | reading command-line arguments via `Sys.args()` |
| `examples/PathDemo.cmm` | `__FILE__`/`__DIR__` constants and `Sys.cwd`/`Sys.chdir` |
| `examples/LambdaHandler.cmm` | AWS Lambda custom-runtime handler (`Lambda.*`, `Sys.peakRss`) |
| `examples/Deploy.cmm`   | build a Zip and deploy it with `Lambda.create` (SigV4) |
| `examples/AwsSign.cmm`  | AWS Signature V4 signing (`use Aws;`) vs the official test vector |
| `examples/S3Demo.cmm`   | S3 request signing + presigned URLs (`use S3;`) |
| `examples/MysqlDemo.cmm`| native `Mysql` client — query/exec + async via `run`/`wait` |
| `examples/DateDemo.cmm` | PHP-style `Date.date` / `Date.gmdate` formatting |
| `examples/PregDemo.cmm` | PHP-style regex: `Preg.match`/`replace`/`split`/`test`/`quote` |

Run them all:

```sh
for f in Hello Counter Demo Worker Bank DataDemo; do
    echo "== $f =="; python3 -m cmmc run examples/$f.cmm
done
```

## Standard library

The standard library lives in `stdlib/` and is **written in cmm itself**, layered
over a small set of native primitives. `use Str;` (etc.) resolves automatically
from the bundled `stdlib/` next to the compiler, or from any directory on the
`CMMC_PATH` environment variable (`:`-separated on Unix, `;` on Windows).

Library classes expose **static methods** — call them on the class name, no
instance needed:

```
use Str;
use Arr;
use Net;
use Out;

caps   = Str.repeat("=", 20);            // "===================="
joined = Arr.join(Arr.range(1, 5), " ");  // "1 2 3 4"
text   = Net.get("https://example.com");  // URL  -> HTTP GET body
local  = Net.get("notes.txt");            // path -> file contents
Out.line(joined);                         // flexible print of any value
```

| Class | What it provides |
|-------|------------------|
| `Str` | `repeat`, `join`/`implode`, `explode`, `replaceAll`, `count`, `reverse`, `padLeft`/`padRight`, `capitalize` (basic ops like `upper`/`lower`/`trim`/`split`/`contains` are native methods on `String`) |
| `Arr` | `range`, `first`, `last`, `isEmpty`, `reverse`, `concat`, `slice`, `indexOf`, `contains`, `push`, `unique`, `sum`, `join` (generic over `List[Data]`) |
| `Net` | transparent file-or-URL I/O: `get`, `put`, `post` (url), `delete`, `patch` (url), each with a `*With(headers)` variant taking `List[String]` of `"Key: Value"` lines |
| `Out` | `print` / `line` / `dump` (JSON) — accept any value type |

These rest on native primitives added for the library: the `Sys` namespace
(`Sys.exit(code)`, `Sys.exec(cmd)` capturing stdout, `Sys.shell(cmd)` with
inherited stdio returning the exit code, `Sys.env(name)`, `Sys.args()` returning
the command-line arguments as a `List[String]`, `Sys.cwd()` / `Sys.chdir(path)`
for the working directory, `Sys.peakRss()` for peak memory), plus a built-in
`Lambda` namespace that runs a compiled program as an AWS Lambda `bootstrap`
(and can `Zip.build` a package and deploy it via `Lambda.create`/`updateCode`), `Http.request(method,
url, headers, body)`, and `File.delete(path)`.

### Command-line arguments

`Sys.args()` returns the program's arguments as a `List[String]`. As in C / PHP /
Python, `args[0]` is the program name and the user-supplied arguments follow:

```
fn main() {
    args = Sys.args();
    for a in args { Out.line(a); }
}
```

A compiled binary receives them directly (`./myprog one two`). When using the
`run` subcommand, pass program arguments after `--`:

```
cmm run examples/Args.cmm -- one two three
```

**Not yet provided:** `map`/`filter`/`reduce` and other callback-based helpers —
they need first-class functions, which cmm does not have yet.

## JSON — first-class and fast

JSON is built into the runtime, and it leans on cmm's own flexible value model:
`Json.decode` parses straight into an ordinary `Data` tree (the same dict / list /
value structures the language uses everywhere), and `Json.encode` serializes any
such value straight back. There is no separate document type — **the decoded value
*is* a first-class cmm structure** you can index, iterate, and mutate in place,
then hand back to encode.

```
doc = Json.decode(raw);          // -> Data (object/array/value tree)
name = doc.path("user.name");    // dotted path; numeric segments index arrays
user = doc.get("user");          // ordinary dict/list access
langs = user.get("langs");
langs.add("Forth");              // mutate the flexible value in place
out = Json.encode(doc);          // encode straight from the same structure
pretty = Json.pretty(doc);       // 2-space indented
```

| Namespace | Methods |
|-----------|---------|
| `Json` | `decode`/`parse` → `Data`, `encode`/`stringify` → `String`, `pretty` (indented) |
| `Data` (accessors) | `type` (`object`/`array`/`string`/`int`/`float`/`bool`/`null`), `path`, `at`, `get`/`set`/`has`/`keys`/`length`, `add`, `getStr`/`getInt`/`getFloat`/`getBool` (typed, coercing), `isObject`/`isArray`/`isNull` |

The parser and serializer are tuned for throughput: the parser is pointer-based
with a `memcpy` fast path for unescaped strings and integer-vs-float preserved on
decode; the serializer bulk-copies runs that need no escaping, formats integers
directly, and prints floats at shortest round-trip precision. On a 16.7 MB
document (200k objects) this build measures roughly **decode 90 MB/s, encode
120 MB/s** — about **4.2×** and **1.4×** the previous byte-at-a-time
implementation. See `examples/JsonDemo.cmm` for a full decode → navigate → mutate
→ encode round-trip.


## Project layout

```
bootstrap/             native C compiler (no Python dependency)
  cmm.c                the whole frontend in one file (lexer..codegen..driver)
  embed.c              embeds the runtime into the binary
  embedded_runtime.h   generated: runtime sources as byte arrays
  build_cmm.sh/.bat   build the native compiler (needs only a C compiler)
  cmm                  the built binary
cmmc/                 Python compiler package (reference / oracle)
  lexer.py             tokenizer
  parser.py            recursive-descent parser (enforces the grouping rule)
  ast.py               AST node definitions
  types.py             type model + native-class signature tables
  analyzer.py          semantic analysis, type inference, call resolution
  codegen.py           C-- AST -> C
  compiler.py          import resolution + C-compiler driver
  __main__.py          CLI (build / run / emit)
runtime/
  cmm_runtime.h       runtime API
  cmm_runtime.c       regions, tagged values, native classes, threads, sockets, TLS
stdlib/                standard library, written in cmm (Str, Arr, Net, Out)
examples/              sample .cmm programs
build.sh / build.bat   convenience wrappers
```

## Building on Windows

`cmm` compiles through zig, so the only prerequisite is zig itself:

- Install it with `pip install ziglang`, or download a build from ziglang.org
  and put `zig` on PATH (or point `CMMC_ZIG` at it).
- cmm links `-lws2_32 -ladvapi32` (and `-lbcrypt` for TLS) automatically.

Then `python -m cmmc build examples\Demo.cmm` produces `Demo.exe`. The same
`zig cc` toolchain cross-compiles to Windows from Linux/macOS too, via
`cmm build app.cmm --target windows-x64`.


## Build cache

`cmm` caches the compiled runtime object and the vendored TLS library per
target under `~/.cache/cmm` (Linux/macOS) or `%LOCALAPPDATA%\cmm\cache`
(Windows), or `$CMMC_CACHE` if set. The first build of a target compiles the
runtime and (if used) mbedTLS; subsequent builds reuse them, so edit-rebuild
cycles are typically tens of milliseconds instead of seconds. The cache keys on
a hash of the runtime source and TLS config, so it self-invalidates when those
change; delete the cache dir to force a clean rebuild.

## How it works

Values use a single tagged union (`CmmValue`) at runtime, so collections can
hold mixed types and codegen stays simple; the analyzer still infers static
types to pick the right method/dispatch. Each user class becomes a `CmmObject`
with a fields array (indexed by declaration order) plus a per-field mutex array
for `use` locks. Methods compile to `<Class>_<method>(CmmValue self, ...)`.
Operators always call runtime functions (`lang_add`, `lang_lt`, …) so they work
uniformly across Int/Float/String/Data. Each `run` site generates a small thunk
struct and thread function that captures the call's operands by value.

## Known limitations

This is a complete, working implementation of the core language. Remaining
honest caveats:

- **Remote imports** (`use "https://…";`) are parsed and reported but not
  fetched; referenced classes must be available locally. Local `use` works.
- **HTTPS is certificate-verified, out of the box.** `Http.get/post` speak
  `https://` (TLS 1.2/1.3, SNI, HTTP/1.1 with chunked decoding) via a vendored
  mbedTLS that zig compiles for whatever you target — host, Amazon Linux,
  Windows — with no system OpenSSL or other packages. Server certificates are
  verified against a CA bundle compiled into the binary. Set
  `CMMC_TLS_INSECURE=1` at runtime to skip verification; the raw `Socket` class
  remains plain TCP.
- **First TLS build per target is one-time ~15s.** The vendored mbedTLS is
  compiled once for each target and cached (`~/.cache/cmm/tls/<target>/`, or
  `%LOCALAPPDATA%\cmm\cache` on Windows); later builds just link it. Pass
  `--no-tls` to leave TLS out entirely (smaller binary).
- **Cross-region object graphs.** Objects are shared by reference across call
  frames (so `@`locks work across threads). Relocation/copy of nested
  containers assumes acyclic data; cyclic structures are not handled.
- **Jobs are frame-scoped.** `wait` a `Job` within the function that `run`
  started it (as the examples do); a job must not outlive its creating frame.
- The type checker is intentionally lenient (the dynamic value model tolerates
  mismatches); it enforces the spec's structural rules rather than full
  soundness.

### Fixed in this build
- **Per-function memory reclaim** now works: each call gets its own region,
  freed on return after the result is relocated to the caller. A stress test
  that allocates ~13 GB cumulatively peaks at ~11 MB resident
  (`examples/MemReclaim.cmm`).
- **`return` inside a `use` lock auto-unlocks.** Any locks held are released
  (innermost first) before the function returns, so early returns from inside
  a lock body no longer deadlock (`examples/LockReturn.cmm`).
- **TLS / HTTPS** is supported as described above (`examples/HttpsDemo.cmm`).

## License

Provided as-is for the project owner to use and extend.
