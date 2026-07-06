# Aws — Signature Version 4

`use Aws;` — pure-cmm implementation of AWS SigV4 request signing, built on the
`Crypto` natives. Verified byte-for-byte against AWS's official `get-vanilla`
test vector.

## Functions

### `Aws.uriEncode(s: String, encodeSlash: Bool) -> String`
RFC-3986 percent-encoding as AWS expects (uppercase hex). When `encodeSlash` is
false, `/` is preserved (S3 object-key paths).

### `Aws.canonicalQuery(params: Dict[String,String]) -> String`
Sorted, URI-encoded `k=v&k=v` canonical query string.

### `Aws.signHeaders(req: Dict[String,String], headers: Dict[String,String]) -> Dict[String,String]`
Core signer. `req` carries the scalars; `headers` are the HTTP headers to sign
(must include at least `host` and `x-amz-date`).

`req` keys: `method`, `uri`, `query`, `payloadHash`, `region`, `service`,
`accessKey`, `secretKey`, `amzDate` (`YYYYMMDDTHHMMSSZ`), `dateStamp`
(`YYYYMMDD`).

Returns: `authorization`, `signedHeaders`, `signature`, plus the intermediate
`canonicalRequest` / `stringToSign`.

```cmm
use Aws;
headers = {"host": "example.amazonaws.com"};
headers.set("x-amz-date", "20150830T123600Z");
req = {"method": "GET"};
req.set("uri", "/");            req.set("query", "");
req.set("payloadHash", Crypto.sha256Hex(""));
req.set("region", "us-east-1"); req.set("service", "service");
req.set("accessKey", ak);       req.set("secretKey", sk);
req.set("amzDate", "20150830T123600Z"); req.set("dateStamp", "20150830");
res = Aws.signHeaders(req, headers);
Console.println(res.get("authorization"));
```

Real-time timestamps: `Date.amzDate()` returns the current UTC
`YYYYMMDDTHHMMSSZ`; the date-stamp is its first 8 characters.
