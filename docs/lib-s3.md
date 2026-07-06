# S3

`use S3;` — a minimal Amazon S3 client built on `Aws` (SigV4) and `Http.send`.
Virtual-hosted-style requests: `https://<bucket>.s3.<region>.amazonaws.com/<key>`.
Signing is verified against known-answer vectors; authenticated calls need real
AWS credentials at runtime.

## High-level (perform the request)

| Call | Returns |
|------|---------|
| `S3.get(bucket, region, key, ak, sk)` | `Data` `{status, body}` |
| `S3.put(bucket, region, key, body, ak, sk)` | `Data` `{status, body}` |
| `S3.delete(bucket, region, key, ak, sk)` | `Data` `{status, body}` |
| `S3.list(bucket, region, prefix, ak, sk)` | `Data` `{status, body}` (ListObjectsV2 XML) |

These fetch the current UTC time via `Date.amzDate()` and sign automatically.

## Lower-level

- `S3.buildHeaders(bucket, region, method, key, query, body, ak, sk, amzDate, dateStamp) -> Dict` —
  returns `host`, `x-amz-date`, `x-amz-content-sha256`, `authorization`.
- `S3.presignGet(bucket, region, key, expires, ak, sk, amzDate, dateStamp) -> String` —
  a query-string-signed (SigV4) presigned URL.

```cmm
use S3;
resp = S3.get("my-bucket", "us-east-1", "path/to/object.txt", ak, sk);
Console.println(resp.getInt("status"));
Console.println(resp.getStr("body"));

url = S3.presignGet("my-bucket", "us-east-1", "file.txt", "3600", ak, sk,
                    Date.amzDate(), Date.amzDate().substring(0, 8));
```
