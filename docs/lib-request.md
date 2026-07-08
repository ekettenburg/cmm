# Request handling + local dev server

`Http.parseRequest(event)` turns a **Lambda Function URL** event (the JSON string
`Lambda.next()` returns, payload format 2.0) into one normalized request object,
so your handler reads bodies, query vars, headers, cookies, and file uploads the
same way regardless of content type. (No API Gateway required — this targets
Function URLs directly.)

## Normalized request

`Http.parseRequest(eventJson) -> Data`:

```
{
  "method": "POST",
  "path": "/users/5",
  "query":   { ...raw query string, percent-decoded... },
  "headers": { ...all request headers... },
  "cookies": { ...parsed from the Cookie header / v2 cookies... },
  "contentType": "application/json",
  "body": "<raw decoded body>",
  "json": { ... } | empty,     // Content-Type: application/json
  "form": { ... } | empty,     // application/x-www-form-urlencoded OR multipart fields
  "files": [                    // multipart/form-data file parts
    { "name": "avatar", "filename": "a.png", "contentType": "image/png",
      "size": 1234, "data": "<raw bytes>" }
  ]
}
```

Base64-encoded bodies (`isBase64Encoded: true`, as Function URLs send for binary)
are decoded automatically. JSON, URL-encoded form, and multipart uploads are all
decoded from `Content-Type`; file bytes are delivered raw and binary-safe.

## Request library (ergonomic accessors)

`use Request;`:

```
m    = Request.method(req);        p    = Request.path(req);
name = Request.query(req, "name"); auth = Request.header(req, "authorization");
sid  = Request.cookie(req, "session");   who = Request.form(req, "name");
body = Request.json(req);          fs   = Request.files(req);
post = Request.isMethod(req, "POST");
```

## Local dev server — same paths as the Function URL, multi-threaded

The `Serve` namespace runs a real HTTP server on localhost that produces the
**same event shape** the Function URL delivers, so the code between the source
and the response is identical in both environments:

```
Lambda:  ev = Lambda.next();  ...  Lambda.success(resp);
Local:   ev = Serve.next();   ...  Serve.respond(resp);
```

- `Serve.listen(port) -> Bool` — bind + listen on `127.0.0.1:port` (once).
- `Serve.next() -> String` — accept a connection and return a Function-URL v2.0
  event JSON (binary body base64-encoded, exactly like the platform).
- `Serve.respond(resp) -> Bool` — `resp` is a `{statusCode, headers, body}` JSON
  string (the same shape you return to Lambda); written back as the HTTP
  response. Set `isBase64Encoded: true` to send binary.

The current connection is **thread-local**, so the server scales across a pool
of worker threads with the built-in `run`/`wait` — each worker runs the ordinary
next/respond loop and they accept concurrently:

```
fn worker() {
    loop = true;
    while (loop) {
        ev   = Serve.next();
        req  = Http.parseRequest(ev);
        resp = handle(req);
        Serve.respond(resp);
    }
}

fn main() -> Int {
    Serve.listen(8080);
    j1 = run worker();  j2 = run worker();
    j3 = run worker();  j4 = run worker();
    wait j1; wait j2; wait j3; wait j4;
    return 0;
}
```

Swap `Serve.next()`/`Serve.respond()` for `Lambda.next()`/`Lambda.success()` and
the same `handle()` runs on the Function URL. See
[examples/DevServer.cmm](../examples/DevServer.cmm). Verified end-to-end (GET
query, JSON, URL-encoded form, multipart upload) and under concurrent load
against a 4-worker pool.
