# Crypto & Base64

Always available, no import required. Implemented as a standalone hash core in
the runtime (independent of the TLS build), so these work with `--no-tls`.

## Crypto

| Call | Returns | Notes |
|------|---------|-------|
| `Crypto.sha256Hex(data)` | `String` | lowercase hex SHA-256 |
| `Crypto.sha1Hex(data)` | `String` | lowercase hex SHA-1 |
| `Crypto.hmacSha256(key, data)` | `String` | raw 32-byte HMAC (binary-safe string) |
| `Crypto.hmacSha256Hex(key, data)` | `String` | lowercase hex HMAC-SHA256 |
| `Crypto.hex(data)` | `String` | lowercase hex of the input bytes |
| `Crypto.randomHex(nbytes)` | `String` | CSPRNG (`/dev/urandom`; `CryptGenRandom` on Windows), hex of `nbytes` bytes |

Strings are byte buffers, so `key`/`data` may contain arbitrary bytes (e.g. the
raw output of `hmacSha256` can be fed straight back in as a key).

## Base64

| Call | Returns |
|------|---------|
| `Base64.encode(data)` | `String` (standard `+/` alphabet, padded) |
| `Base64.decode(str)` | `String` (ignores non-alphabet characters) |

```cmm
sig = Crypto.hmacSha256Hex("key", "message");
tok = Base64.encode(Crypto.hmacSha256("key", "message"));
```
