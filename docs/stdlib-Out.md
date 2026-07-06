# Out (standard library)

Convenient output that accepts **any** value type (numbers, bools, lists,
dicts, `Data`) and stringifies it. Import with `use Out;`.

| Method | Signature | Description |
|--------|-----------|-------------|
| `print` | `(x) -> Bool` | write `x` with no trailing newline |
| `line` | `(x) -> Bool` | write `x` followed by a newline |
| `dump` | `(x) -> Bool` | write `x` as JSON (useful for lists/dicts/Data) |

```
use Out;

Out.line("hello");
Out.line(42);
Out.print("x = ");
Out.line(3.14);
Out.dump({"a": [1, 2, 3], "b": true});   // JSON form
```
