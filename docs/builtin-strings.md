# Strings (built-in)

`String` is an immutable UTF-8 text value. These methods are built into the
runtime and called directly on any string value. For higher-level helpers
(`repeat`, `join`, `padLeft`, …) see the [`Str`](stdlib-Str.md) standard library.

| Method | Signature | Description |
|--------|-----------|-------------|
| `length` | `() -> Int` | number of bytes |
| `substring` | `(start: Int, end: Int) -> String` | substring `[start, end)` |
| `indexOf` | `(sub: String) -> Int` | first index of `sub`, or `-1` |
| `contains` | `(sub: String) -> Bool` | does the string contain `sub` |
| `startsWith` | `(prefix: String) -> Bool` | prefix test |
| `endsWith` | `(suffix: String) -> Bool` | suffix test |
| `upper` | `() -> String` | uppercase copy |
| `lower` | `() -> String` | lowercase copy |
| `trim` | `() -> String` | strip leading/trailing whitespace |
| `split` | `(sep: String) -> List[String]` | split on `sep` |
| `replace` | `(find: String, rep: String) -> String` | replace all occurrences |
| `toInt` | `() -> Int` | parse as integer |
| `toFloat` | `() -> Float` | parse as float |
| `toStr` | `() -> String` | identity (useful generically) |

```
s = "  Hello, World  ";
t = s.trim();              // "Hello, World"
parts = t.split(", ");     // ["Hello", "World"]
yes = t.contains("World"); // true
n = "42".toInt();          // 42
```

Concatenate with `+`; non-string operands are stringified automatically:

```
msg = ("count = " + 7);    // "count = 7"
```

Scalar conversions also exist on the numeric/bool types: `Int.toStr` /
`Int.toFloat`, `Float.toStr` / `Float.toInt`, `Bool.toStr`.
