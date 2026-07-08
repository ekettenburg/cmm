# Mysql

Always available, no import required. A native MySQL/MariaDB wire-protocol
client: `mysql_native_password` auth and the text protocol (`COM_QUERY`).
Verified end-to-end against MariaDB.

## Functions

| Call | Returns | Notes |
|------|---------|-------|
| `Mysql.connect(host, port, user, pass, db)` | `Int` | connection handle ≥ 0, or -1 on failure |
| `Mysql.query(conn, sql)` | `List[Data]` | one `Data` dict per row (column name → value string) |
| `Mysql.exec(conn, sql)` | `Int` | affected rows for INSERT/UPDATE/DELETE (-1 on error) |
| `Mysql.insertId(conn)` | `Int` | last `AUTO_INCREMENT` id |
| `Mysql.affected(conn)` | `Int` | affected rows of the last statement |
| `Mysql.error(conn)` | `String` | last error text (empty if none) |
| `Mysql.close(conn)` | `Bool` | sends `COM_QUIT` and closes the socket |

Values arrive as strings (text protocol); convert with `.toInt()` / `.toFloat()`
as needed. `NULL` becomes an empty value.

```cmm
c = Mysql.connect("127.0.0.1", 3306, "user", "pass", "app");
rows = Mysql.query(c, "SELECT id, name FROM users ORDER BY id");
for row in rows {
    Console.println(row.getStr("name"));
}
n = Mysql.exec(c, "UPDATE users SET age = age + 1 WHERE id = 1");
Console.println(Mysql.insertId(c));
Mysql.close(c);
```

## Async queries

A single connection cannot run concurrent queries, but separate connections can.
Combine `run`/`wait` with one connection per job for true parallelism:

```cmm
fn fetch(conn: Int, sql: String) -> List[Data] {
    r = Mysql.query(conn, sql);
    return r;
}
c1 = Mysql.connect("127.0.0.1", 3306, "u", "p", "app");
c2 = Mysql.connect("127.0.0.1", 3306, "u", "p", "app");
j1 = run fetch(c1, "SELECT ... ");
j2 = run fetch(c2, "SELECT ... ");
a = wait j1;
b = wait j2;
```

**Coverage / limits:** `mysql_native_password` + text protocol are implemented
and tested. `caching_sha2_password` (MySQL 8's default; needs TLS-cleartext or
RSA) and prepared statements / the binary protocol are not yet supported. Open a
connection per concurrent job.

## TLS / encrypted connections (TiDB Cloud, PlanetScale, RDS)

Use `Mysql.connectTls(host, port, user, pass, db, ca)` for servers that require
TLS. It performs the MySQL protocol's STARTTLS upgrade — read the server
handshake in the clear, send an `SSLRequest`, complete the TLS handshake, then
send credentials over the encrypted channel — and verifies the server
certificate before authenticating.

The `ca` argument selects the trust anchor:

- **A PEM string** — e.g. `File.read("ca.pem")`; the server is verified against
  that CA. Use this with a CA file downloaded from your provider.
- **A file path** to a `.pem` — read for you if the string isn't already PEM.
- **`""`** — verify against cmm's built-in Mozilla root bundle. This works when
  the server presents a publicly-trusted certificate, which **TiDB Cloud** does,
  so `""` is usually enough there.

```
// TiDB Cloud, verifying against the built-in public roots:
conn = Mysql.connectTls("gateway01.<region>.prod.aws.tidbcloud.com", 4000,
                        "<prefix>.root", "<password>", "test", "");

// Or pin a specific CA you downloaded:
ca   = File.read("ca.pem");
conn = Mysql.connectTls(host, 4000, user, pass, db, ca);
```

Hostname is checked against the certificate's SAN, so `host` must match the
name on the cert. Setting `CMMC_TLS_INSECURE=1` in the environment skips
verification (testing only). Everything after connect — `query`, `exec`,
`insertId`, `affected`, `close`, and `run`/`wait` async — is identical to a
plaintext connection; the encryption is transparent. Verified end-to-end against
a `REQUIRE SSL` MariaDB with certificate verification. See
[examples/MysqlTlsDemo.cmm](../examples/MysqlTlsDemo.cmm).
