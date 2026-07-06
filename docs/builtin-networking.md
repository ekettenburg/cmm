# Networking: Http and Socket (built-in)

For everyday fetch/post — including transparent file-or-URL access — prefer the
[`Net`](stdlib-Net.md) standard library. These are the built-in primitives it
rests on.

> TLS note: HTTPS is **certificate-verified** against the full Mozilla CA root
> set (150 roots), DER-packed and compiled into the binary (vendored mbedTLS,
> compiled by zig for any target). No OpenSSL or system packages needed. Set
> `CMMC_TLS_INSECURE=1` at runtime to skip checks on a normally-built binary.
>
> Build `--no-verify` for the smallest binary: the CA roots are left out of the
> binary entirely and peer certificates are **not** checked. This saves ~160 KB
> (a cert-verified HTTPS binary is ~350 KB; `--no-verify` is ~185 KB). Use it
> only when the transport is already trusted or verification is handled
> elsewhere — every host, not just AWS, goes unverified.

> Footprint: mbedTLS is built with a trimmed **client-only** profile
> (`third_party/cmm_mbedtls_config.h`) — TLS 1.2, ECDHE + AES-GCM, full X.509
> chain verification, and nothing else. Server TLS, TLS 1.3/PSA, DTLS, DHE, the
> legacy cipher suites (DES/ARIA/Camellia/ChaCha/CBC), X.509 writing/CSR/CRL,
> and PKCS#7/12 are all compiled out. A cert-verified HTTPS binary is ~410 KB
> vs ~790 KB with the stock config.
>
> TLS is only linked when a program actually calls `Http.*` (the default
> `--tls auto`); pass `--no-tls` to force it off, or `--tls` to force it on. A
> Lambda handler that only polls the runtime API (plain localhost HTTP) and
> makes no outbound HTTPS calls builds to ~17 KB with no TLS at all. To go
> further, prune `third_party/mbedtls/sources.list` to skip compiling the files
> the trimmed config disables (cuts the one-time TLS build; no effect on binary
> size, which `--gc-sections` already handles).

## Http

| Method | Signature | Description |
|--------|-----------|-------------|
| `get` | `(url: String) -> String` | GET, return the body |
| `post` | `(url: String, body: String) -> String` | POST, return the body |
| `request` | `(method: String, url: String, headers: List[String], body: String) -> String` | full request; `headers` are `"Key: Value"` lines, `body` `""` for none |

```
body = Http.get("https://example.com/api");
hdrs = ["Accept: application/json", "X-Token: abc"];
resp = Http.request("POST", "https://example.com/api", hdrs, "{\"a\":1}");
```

## Socket

A TCP socket handle, obtained from `Socket.connect`.

| Call | Signature | Description |
|------|-----------|-------------|
| `Socket.connect` | `(host: String, port: Int) -> Socket` | open a connection |
| `sock.write` | `(data: String) -> Bool` | send bytes |
| `sock.read` | `(n: Int) -> String` | read up to `n` bytes |
| `sock.readAll` | `() -> String` | read until the peer closes |
| `sock.close` | `() -> Bool` | close the socket |

```
s = Socket.connect("example.com", 80);
s.write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
resp = s.readAll();
s.close();
```
