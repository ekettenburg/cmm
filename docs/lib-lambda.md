# AWS Lambda (custom runtime)

Run a compiled cmm program as the `bootstrap` executable of a Lambda function on
the `provided.al2` / `provided.al2023` runtimes. The `Lambda` namespace is a thin
client over the AWS Lambda Runtime API (plain HTTP at `$AWS_LAMBDA_RUNTIME_API`)
— it reuses cmm's existing socket/HTTP code and does **not** reimplement the AWS
runtime. `Lambda` is built in; no import is required.

## The model

A custom runtime is a loop: ask the Runtime API for the next invocation, run
your handler, post the result, repeat. Anything written to stdout/stderr is
captured by Lambda and sent to **CloudWatch Logs**.

```
fn main() {
    while (true) {
        serveOne();        // one function call per invocation (see "Memory")
    }
}

fn serveOne() {
    event = Lambda.next();         // blocks until the next invocation arrives
    result = handle(event);        // your logic -> a response string
    ok = Lambda.success(result);   // post the response
    Lambda.log(("peak_rss_bytes=" + Sys.peakRss()));
}
```

## API

| Call | Signature | Description |
|------|-----------|-------------|
| `Lambda.next` | `() -> String` | block for the next invocation; returns the event body and captures the request context |
| `Lambda.success` | `(body: String) -> Bool` | post a successful response for the current invocation |
| `Lambda.failure` | `(errorType: String, errorMessage: String) -> Bool` | post an invocation error (also sets the error-type header) |
| `Lambda.initError` | `(errorType: String, errorMessage: String) -> Bool` | report a fatal initialization error |
| `Lambda.requestId` | `() -> String` | request id of the current invocation |
| `Lambda.deadlineMs` | `() -> Int` | invocation deadline (epoch ms) |
| `Lambda.invokedArn` | `() -> String` | the invoked function ARN |
| `Lambda.traceId` | `() -> String` | the X-Ray trace id (also exported as `_X_AMZN_TRACE_ID`) |
| `Lambda.log` | `(msg: String) -> Void` | write a line to stderr → CloudWatch |
| `Sys.peakRss` | `() -> Int` | peak resident memory of the process, in bytes |

The event body is plain text — decode it with `Json.decode` and build the
response with `Json.encode` (see [JSON / Data](builtin-json.md)).

## Logging to CloudWatch

stdout and stderr are both streamed to CloudWatch. `Out.line` / `Out.dump`
(stdout) and `Lambda.log` (stderr) all show up there; CloudWatch adds the
timestamp per line. Log peak memory per invocation with `Sys.peakRss()`.

## Memory

The runtime reclaims a function's memory when it **returns**. The bootstrap loop
never returns, so put each invocation's work in a helper (like `serveOne`) that
returns every iteration — then the event, the decoded `Data`, and the response
are all reclaimed per invocation and resident memory stays flat across a
long-lived container. `Sys.peakRss()` lets you watch that the high-water mark
holds steady.

## Error handling

cmm has no exceptions, so decide success vs. failure from your handler's result
and branch:

```
fn serveOne() {
    event = Lambda.next();
    result = handle(event);
    bad = (result == "");
    if (bad) {
        ok = Lambda.failure("HandlerError", "handler produced no result");
    } else {
        ok = Lambda.success(result);
    }
}
```

`Lambda.failure` posts `{"errorType":...,"errorMessage":...}` to the Runtime API
and sets `Lambda-Runtime-Function-Error-Type`. Use `Lambda.initError` for a
fatal error during one-time setup before the loop.

## Deploying from cmm

You can also build and upload the package from a cmm program using the
[`Zip`](lib-zip.md) library and these control-plane calls. They sign requests
with AWS SigV4 over HTTPS, so the deploying program must be a **TLS build**
(`cmmc build deploy.cmm --tls`). Region and credentials are read from the
standard environment variables: `AWS_REGION` (or `AWS_DEFAULT_REGION`),
`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, and `AWS_SESSION_TOKEN` (optional).

| Call | Signature | Description |
|------|-----------|-------------|
| `Lambda.create` | `(name: String, role: String, zip: String) -> String` | create a function (Runtime `provided.al2023`, Handler `bootstrap`); returns the API response JSON |
| `Lambda.updateCode` | `(name: String, zip: String) -> String` | replace an existing function's code; returns the API response JSON |

```
code = File.read("bootstrap");
entries = [];
e = {"name": "bootstrap", "content": code, "exec": true};
entries.add(e);
pkg = Zip.build(entries);

role = Sys.env("LAMBDA_ROLE_ARN");
resp = Lambda.create("cmm-greeter", role, pkg);   // or Lambda.updateCode(name, pkg)
Out.line(resp);                                    // decode with Json.decode to inspect
```

Both calls return the raw response body; on failure the JSON contains an
`errorMessage`/`Message` field. `Lambda.create` uses sensible defaults
(`provided.al2023`, handler `bootstrap`, Zip package); for other settings,
create the function once with the AWS CLI and then push code with
`Lambda.updateCode`. See `examples/Deploy.cmm`.

## Deploying

```
cmmc build LambdaHandler.cmm -o bootstrap     # the executable MUST be named "bootstrap"
chmod +x bootstrap
zip function.zip bootstrap
# upload function.zip to a function using the provided.al2023 (or provided.al2) runtime
```

Build on a Linux toolchain that matches the Lambda environment (Amazon Linux,
x86-64). A statically-linked or musl build avoids glibc-version surprises; a
plain `gcc` build on a recent Amazon Linux 2023 box works directly.

See `examples/LambdaHandler.cmm` for the complete handler used above.
