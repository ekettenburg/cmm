# Str (standard library)

String helpers layered on the built-in [`String`](builtin-strings.md) methods.
Written in cmm itself. Import with `use Str;` and call statically: `Str.method(...)`.

| Method | Signature | Description |
|--------|-----------|-------------|
| `repeat` | `(s: String, n: Int) -> String` | `s` repeated `n` times |
| `join` | `(parts: List[String], sep: String) -> String` | join with `sep` |
| `implode` | `(parts: List[String], sep: String) -> String` | alias of `join` |
| `explode` | `(s: String, sep: String) -> List[String]` | split on `sep` |
| `replaceAll` | `(s: String, find: String, rep: String) -> String` | replace every occurrence |
| `count` | `(s: String, sub: String) -> Int` | number of occurrences of `sub` |
| `reverse` | `(s: String) -> String` | reversed copy |
| `padLeft` | `(s: String, width: Int, ch: String) -> String` | left-pad to `width` |
| `padRight` | `(s: String, width: Int, ch: String) -> String` | right-pad to `width` |
| `capitalize` | `(s: String) -> String` | uppercase the first character |

```
use Str;

line = Str.repeat("=", 20);
csv = Str.join(["a", "b", "c"], ",");      // "a,b,c"
cells = Str.explode("a,b,c", ",");         // ["a","b","c"]
num = Str.padLeft("7", 3, "0");            // "007"
```
