# Collections: List and Dict (built-in)

cmm has two built-in containers plus the dynamic `Data` type (documented with
[JSON](builtin-json.md)). For functional-style helpers over lists see the
[`Arr`](stdlib-Arr.md) standard library.

## List[T]

An ordered, growable sequence. Iterate with `for x in list`.

| Method | Signature | Description |
|--------|-----------|-------------|
| `add` | `(value: T) -> Void` | append |
| `get` | `(i: Int) -> T` | element at `i` (`empty` if out of range) |
| `set` | `(i: Int, value: T) -> Void` | overwrite element `i` |
| `remove` | `(i: Int) -> Void` | remove element `i` |
| `length` | `() -> Int` | element count |
| `contains` | `(value: T) -> Bool` | membership test |
| `clear` | `() -> Void` | empty the list |

```
xs = [10, 20, 30];
xs.add(40);
n = xs.length();        // 4
first = xs.get(0);      // 10
for v in xs {
    Out.line(v);
}
```

List literals: `[a, b, c]` (empty: `[]`).

## Dict[V]

A string-keyed map.

| Method | Signature | Description |
|--------|-----------|-------------|
| `get` | `(key: String) -> V` | value for `key` (`empty` if absent) |
| `set` | `(key: String, value: V) -> Void` | insert / update |
| `has` | `(key: String) -> Bool` | key present |
| `remove` | `(key: String) -> Void` | delete a key |
| `keys` | `() -> List[String]` | all keys |
| `length` | `() -> Int` | entry count |

```
m = {"a": 1, "b": 2};
m.set("c", 3);
present = m.has("a");   // true
for k in m.keys() {
    Out.line(k);
}
```

Dict literals: `{"k": v, ...}` (empty: `{}`).

Reading a missing key or out-of-range index returns `empty`, so guard with
`(v == empty)` when a lookup may miss.
