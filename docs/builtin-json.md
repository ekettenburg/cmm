# JSON and the Data type (built-in)

JSON in cmm is built on the language's own flexible value model. `Json.decode`
parses text into an ordinary `Data` tree (the same dict / list / value
structures used everywhere), and `Json.encode` serializes any such value back.
There is no separate document type — **the decoded value is a first-class cmm
structure** you index, navigate, and mutate, then hand back to encode.

The parser and serializer are tuned for throughput (pointer-based parse with a
`memcpy` fast path for unescaped strings; bulk-copying escape-aware serializer;
shortest round-trip floats). On a 16.7 MB / 200k-object document this build
measures roughly decode 90 MB/s, encode 120 MB/s.

## Json namespace

| Method | Signature | Description |
|--------|-----------|-------------|
| `decode` / `parse` | `(text: String) -> Data` | parse JSON into a `Data` tree |
| `encode` / `stringify` | `(value: Data) -> String` | serialize compactly |
| `pretty` | `(value: Data) -> String` | serialize with 2-space indent |

## Data accessors

`Data` is the dynamic value. It can hold an object, array, string, number,
bool, or null, and behaves as the matching container at runtime.

| Method | Signature | Description |
|--------|-----------|-------------|
| `type` | `() -> String` | `"object"`/`"array"`/`"string"`/`"int"`/`"float"`/`"bool"`/`"null"` |
| `isObject` / `isArray` / `isNull` | `() -> Bool` | shape tests |
| `get` | `(key: String) -> Data` | object field (`empty` if absent) |
| `set` | `(key: String, value: Data) -> Void` | set an object field |
| `has` | `(key: String) -> Bool` | object has key |
| `keys` | `() -> List[String]` | object keys |
| `at` | `(i: Int) -> Data` | array element |
| `add` | `(value: Data) -> Void` | append (when the value is an array) |
| `length` | `() -> Int` | element / entry count |
| `path` | `(p: String) -> Data` | dotted path; numeric segments index arrays |
| `getStr` | `(key: String) -> String` | field coerced to String |
| `getInt` | `(key: String) -> Int` | field coerced to Int |
| `getFloat` | `(key: String) -> Float` | field coerced to Float |
| `getBool` | `(key: String) -> Bool` | field coerced to Bool |

```
raw = "{\"user\":{\"name\":\"Ada\",\"langs\":[\"C--\",\"Lisp\"]}}";
doc = Json.decode(raw);

name = doc.path("user.name");      // "Ada"
lang0 = doc.path("user.langs.0");  // "C--"

user = doc.get("user");
langs = user.get("langs");
langs.add("Forth");                // mutate in place

out = Json.encode(doc);            // encode straight from the same tree
pretty = Json.pretty(doc);
```

See `examples/JsonDemo.cmm` for a full decode → navigate → mutate → encode
round-trip.
