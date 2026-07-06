# Building for Amazon Linux 2023 (AWS Lambda)

cmm uses **one** C backend everywhere: [zig](https://ziglang.org). The same
`cmmc` on Windows or Linux can produce a native host binary *and* cross-compile
a binary for Amazon Linux 2023 — the OS behind the Lambda `provided.al2023`
runtime. No Docker, no per-platform toolchain juggling.

## 1. Install zig once

zig ships as a single self-contained cross-compiler. The easiest install works
identically on Windows and Linux:

```
pip install ziglang
```

`cmmc` auto-discovers it (it also accepts a real `zig` on `PATH`, or
`CMMC_ZIG=/path/to/zig`). That's the only prerequisite.

## 2. Build for Amazon Linux 2023

```
cmmc build handler.cmm -o bootstrap --target al2023
```

This emits an x86-64 ELF linked against **glibc 2.34** — exactly what AL2023
provides — so it runs there unchanged. The command is byte-for-byte the same on
Windows and Linux; only the produced binary differs.

Targets:

| `--target`       | Result                                             |
|------------------|----------------------------------------------------|
| `al2023`         | x86-64, glibc 2.34 (Lambda default). `--lambda` is an alias. |
| `al2023-arm64`   | arm64 / Graviton, glibc 2.34                        |
| `al2023-static`  | fully static (musl) — runs on *any* Linux, zero deps |

Lambda's custom runtime expects the executable to be named `bootstrap`, so use
`-o bootstrap`.

## 3. Package and deploy

`bootstrap` is the whole function. Zip it and ship it — cmm can do both itself
(see [`lib-lambda.md`](lib-lambda.md) and [`lib-zip.md`](lib-zip.md)):

```
cmmc run deploy.cmm        # File.read -> Zip.build -> SigV4 -> Lambda.create/updateCode
```

…or use the AWS CLI:

```
zip function.zip bootstrap
aws lambda create-function --function-name my-fn \
    --runtime provided.al2023 --handler bootstrap \
    --role arn:aws:iam::ACCT:role/my-role \
    --zip-file fileb://function.zip
```

## Verifying the binary (optional)

```
file bootstrap          # ELF 64-bit LSB executable, x86-64
objdump -T bootstrap | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1   # <= 2.34
```

The highest required `GLIBC_` version must be `2.34` or lower for AL2023.

## TLS / HTTPS

HTTPS works out of the box in cross-compiled binaries. cmm vendors
[mbedTLS](https://www.trustedfirmware.org/projects/mbed-tls/) and zig compiles
it for whatever target you build — host, AL2023, arm64, Windows — so there is
no system OpenSSL to install and nothing to configure. Certificates are
verified against a CA bundle that is compiled into the binary, so a handler can
call AWS APIs or third-party endpoints over real, verified TLS with no extra
files in the deployment package.

The first TLS build for a given target compiles the vendored mbedTLS once
(~15s) and caches it under your user cache dir
(`~/.cache/cmm/tls/<target>/`, or `%LOCALAPPDATA%\cmm\cache` on Windows);
subsequent builds just link it and are instant.

To skip certificate verification (e.g. hitting an endpoint with a private CA),
set `CMMC_TLS_INSECURE=1` in the binary's environment at runtime.

If you'd rather not bundle TLS at all (smaller binary, runtime-API-only
handler), pass `--no-tls`.

## Why zig

One download, every host OS, every Lambda arch, no sysroot wrangling, and it
pins the glibc version so binaries built on a newer dev machine still run on
AL2023. It also doubles as cmm's normal host compiler — if zig is installed,
`cmmc build foo.cmm` uses it for local builds too, so there is genuinely just
one toolchain to think about.
