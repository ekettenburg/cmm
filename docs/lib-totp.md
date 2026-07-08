# Totp — zero-write one-time codes

Server-generated one-time login codes (RFC 6238-style, **60-second** step) with
no per-user storage. Each user's seed is derived on the fly from their identity
plus a single site-wide secret, so there's nothing to persist or revoke per user
— rotate the secret to invalidate everyone at once.

## API — `use Totp;`

- `Totp.seed(text, secret) -> String` — the per-user seed, `HMAC-SHA256(secret,
  lowercase(trim(text)))` (32 raw bytes). `text` is the user identity (email,
  user id); `secret` is your server key. Recompute it per request; store nothing.
- `Totp.code(seed) -> String` — the current 6-digit code (what you send by
  email/SMS).
- `Totp.minute(seed, offsetSteps, digits) -> String` — code for the current
  60-second window shifted by `offsetSteps` minutes, `digits` long.
- `Totp.verify(seed, userCode, periods) -> Bool` — constant-time check of the
  submitted code against the current minute and the previous `periods - 1`
  minutes. `periods = 11` tolerates a code up to ~10 minutes old.

## Login flow

```
use Totp;

// 1. When the user requests a code:
seed = Totp.seed(email, siteSecret);
code = Totp.code(seed);
// ... send `code` to the user by email/SMS ...

// 2. When the user submits it back:
seed = Totp.seed(email, siteSecret);          // re-derived, nothing stored
if (Totp.verify(seed, submitted, 11)) {
    // ... issue a session ...
}
```

## Underlying primitives (built-in `Crypto`)

- `Crypto.hmacSha1(key, data) -> String` — raw 20-byte HMAC-SHA1.
- `Crypto.hotp(seed, counter, digits) -> String` — RFC 4226 HOTP.
- `Crypto.timingSafeEqual(a, b) -> Bool` — constant-time compare (like PHP
  `hash_equals`).

The HOTP core is verified against the RFC 4226 test vectors, and the full
seed→code chain matches a reference implementation. See
[examples/TotpDemo.cmm](../examples/TotpDemo.cmm).
