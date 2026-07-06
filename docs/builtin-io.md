# I/O: Console and File (built-in)

For most output prefer the [`Out`](stdlib-Out.md) standard library (it accepts
any value type). `Console` and `File` are the low-level built-ins.

## Console

| Method | Signature | Description |
|--------|-----------|-------------|
| `print` | `(x) -> Void` | write without a trailing newline |
| `println` | `(x) -> Void` | write with a trailing newline |
| `read` | `() -> String` | read a line from stdin |

```
Console.print("name: ");
name = Console.read();
Console.println(("hello " + name));
```

## File

| Method | Signature | Description |
|--------|-----------|-------------|
| `read` | `(path: String) -> String` | whole-file contents |
| `write` | `(path: String, data: String) -> Bool` | overwrite; `true` on success |
| `append` | `(path: String, data: String) -> Bool` | append; `true` on success |
| `exists` | `(path: String) -> Bool` | does the path exist |
| `delete` | `(path: String) -> Bool` | remove the file; `true` on success |

```
ok = File.write("/tmp/note.txt", "first line\n");
File.append("/tmp/note.txt", "second line\n");
if (File.exists("/tmp/note.txt")) {
    body = File.read("/tmp/note.txt");
    Out.line(body);
}
```

Combine with [`__DIR__`](builtin-system.md) and `Sys.cwd()` to locate files
relative to the source or working directory.
