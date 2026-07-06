# Arr (standard library)

List helpers, generic over `List[Data]`. Written in cmm. Import with `use Arr;`
and call statically: `Arr.method(...)`. (Callback helpers like `map`/`filter`
are not provided yet — cmm has no first-class functions.)

| Method | Signature | Description |
|--------|-----------|-------------|
| `range` | `(start: Int, end: Int) -> List[Int]` | integers `[start, end)` |
| `first` | `(items: List[Data]) -> Data` | first element (`empty` if none) |
| `last` | `(items: List[Data]) -> Data` | last element (`empty` if none) |
| `isEmpty` | `(items: List[Data]) -> Bool` | length == 0 |
| `reverse` | `(items: List[Data]) -> List[Data]` | reversed copy |
| `concat` | `(a: List[Data], b: List[Data]) -> List[Data]` | a followed by b |
| `slice` | `(items: List[Data], start: Int, end: Int) -> List[Data]` | sublist `[start, end)` |
| `indexOf` | `(items: List[Data], value: Data) -> Int` | index of `value`, or `-1` |
| `contains` | `(items: List[Data], value: Data) -> Bool` | membership test |
| `push` | `(items: List[Data], value: Data) -> List[Data]` | append, returning the list |
| `unique` | `(items: List[Data]) -> List[Data]` | duplicates removed |
| `sum` | `(items: List[Int]) -> Int` | sum of integers |
| `join` | `(items: List[Data], sep: String) -> String` | stringify and join |

```
use Arr;

ns = Arr.range(1, 6);          // [1,2,3,4,5]
total = Arr.sum(ns);           // 15
rev = Arr.reverse(ns);         // [5,4,3,2,1]
csv = Arr.join(ns, ", ");      // "1, 2, 3, 4, 5"
```
