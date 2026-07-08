# Router — path routing for Function URL / dev-server requests

Maps an incoming request (normalized by `Http.parseRequest`) to a handler based
on HTTP method + path pattern, extracting path params.

**Constraint:** cmm has no reflection or function pointers, so the router can't
invoke your classes directly. It returns the matched **handler id** (a string you
choose) plus captured params; you dispatch on that id with a small `if/else` to
the real `class.method`. The router still owns route storage, method matching,
pattern matching, and param extraction.

## API — `use Router;`

- `Router.create() -> List[Data]` — a new empty route table (or declare your own
  `@routes: List[Data];`).
- `Router.add(routes, method, pattern, handler)` and verb helpers
  `Router.get/post/put/patch/delete/any(routes, pattern, handler)` — register a
  route. `handler` is any id string, e.g. `"users.show"`.
- `Router.match(routes, req) -> Dict[String,String]` — the match result.
- `Router.isMatch(m) -> Bool`, `Router.handler(m) -> String`,
  `Router.param(m, name) -> String` — read the result.

Patterns: `/users/{id}/posts/{postId}`. Literal segments must match exactly;
`{name}` captures that segment into a param. Method `"*"` matches any verb.

## Full request loop

```
class App;
use Router;

@routes: List[Data];

fn usersShow(req: Data, m: Dict[String, String]) -> String {
    id = Router.param(m, "id");
    // ... look up user `id`, build a {statusCode,headers,body} JSON ...
    return "...";
}

fn dispatch(req: Data) -> String {
    m = Router.match(@routes, req);
    if (Router.isMatch(m)) {
        h = Router.handler(m);
        if (h == "users.show") { return usersShow(req, m); }
        // ... other handlers ...
    }
    return "{\"statusCode\":404,\"body\":\"not found\"}";
}

fn main() -> Int {
    Router.get(@routes, "/users/{id}", "users.show");
    // ... more routes ...

    loop = true;
    while (loop) {
        ev   = Serve.next();              // Lambda.next() in production
        req  = Http.parseRequest(ev);
        resp = dispatch(req);
        Serve.respond(resp);             // Lambda.success(resp) in production
    }
    return 0;
}
```

The same `dispatch()` runs on the Function URL and the local dev server. See
[examples/RouteDemo.cmm](../examples/RouteDemo.cmm). Verified: GET/POST matching,
single and multi param capture, method-mismatch and no-match both fall through.
