# Net (standard library)

Transparent file-or-URL I/O. Each call accepts either a local path or an
`http(s)://` URL and does the right thing. Written in cmm over the built-in
[`Http`](builtin-networking.md) and `File` primitives. Import with `use Net;`.

| Method | Signature | Description |
|--------|-----------|-------------|
| `isUrl` | `(target: String) -> Bool` | does `target` start with `http://`/`https://` |
| `get` | `(target: String) -> String` | read a file or GET a URL |
| `getWith` | `(url: String, headers: List[String]) -> String` | GET with headers |
| `put` | `(target: String, data: String) -> Bool` | write a file or PUT a URL |
| `putWith` | `(url: String, data: String, headers: List[String]) -> String` | PUT with headers |
| `post` | `(url: String, body: String) -> String` | POST |
| `postWith` | `(url: String, body: String, headers: List[String]) -> String` | POST with headers |
| `patch` | `(url: String, body: String) -> String` | PATCH |
| `patchWith` | `(url: String, body: String, headers: List[String]) -> String` | PATCH with headers |
| `delete` | `(target: String) -> Bool` | delete a file or DELETE a URL |
| `deleteWith` | `(url: String, headers: List[String]) -> String` | DELETE with headers |

Headers are passed as a `List[String]` of `"Key: Value"` lines.

```
use Net;

text = Net.get("notes.txt");                    // local file
page = Net.get("https://example.com");          // URL — same call
hdrs = ["Authorization: Bearer xyz"];
data = Net.getWith("https://api.example.com/x", hdrs);
```

> `put`/`delete` against URLs return best-effort `true` and do not parse HTTP
> status codes. TLS is certificate-verified against a bundled CA store.
