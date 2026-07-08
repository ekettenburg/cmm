# System: Sys namespace, args, and path constants (built-in)

The `Sys` namespace exposes the process environment, the working directory,
command-line arguments, and shell access. Path source constants `__FILE__` /
`__DIR__` are part of the language.

## Sys

| Method | Signature | Description |
|--------|-----------|-------------|
| `args` | `() -> List[String]` | command-line arguments; `args[0]` is the program name |
| `cwd` | `() -> String` | current working directory |
| `chdir` | `(path: String) -> Bool` | change working directory; `true` on success |
| `env` | `(name: String) -> String` | environment variable (`""` if unset) |
| `exec` | `(cmd: String) -> String` | run a shell command, capture stdout |
| `shell` | `(cmd: String) -> Int` | run with inherited stdio, return exit code |
| `exit` | `(code: Int) -> Void` | terminate the process |
| `os` | `() -> String` | OS family: `"linux"`, `"macos"`, `"windows"`, or `"unknown"` |
| `arch` | `() -> String` | CPU arch: `"x86_64"`, `"aarch64"`, `"arm"`, `"x86"`, or `"unknown"` |
| `platform` | `() -> String` | `"<os>-<arch>"`, e.g. `"linux-x86_64"` (built at compile time) |

```
args = Sys.args();
for a in args { Out.line(a); }

home = Sys.env("HOME");
here = Sys.cwd();
ok = Sys.chdir("/tmp");
listing = Sys.exec("ls -1");
plat = Sys.platform();   // e.g. "linux-x86_64"
```

Command-line arguments reach a compiled binary directly (`./prog one two`).
Through the `run` subcommand, pass them after `--`:

```
cmm run examples/Args.cmm -- one two three
```

## __FILE__ and __DIR__

Compile-time constants holding the current source file's path and directory
(the path as resolved by the compiler, normally absolute), analogous to PHP's
magic constants:

```
self = __FILE__;   // /abs/path/to/Thing.cmm
dir  = __DIR__;    // /abs/path/to
```

They are resolved when the file is compiled, so they reflect the build-time
location of the source.
