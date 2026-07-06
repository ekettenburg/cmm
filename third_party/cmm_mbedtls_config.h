/* cmm_mbedtls_config.h — minimal mbedTLS profile for cmm.
 *
 * Target: outbound TLS 1.2 *client* to AWS/modern HTTPS endpoints with full
 * X.509 chain verification. Everything not needed for that is compiled out.
 *
 * Dropped vs stock: TLS server, all of TLS 1.3 + PSA crypto, DTLS, session
 * tickets/cache, DHE/FFDH, DES/3DES/ARIA/Camellia/CCM/ChaCha, CBC-mode &
 * MD5/RIPEMD legacy suites, ECJPAKE, LMS/LMOTS, PKCS7/PKCS12, X.509
 * writing/CSR/CRL, and error-string tables.
 *
 * Enable with: -DMBEDTLS_CONFIG_FILE='<cmm_mbedtls_config.h>'
 */
#ifndef CMM_MBEDTLS_CONFIG_H
#define CMM_MBEDTLS_CONFIG_H

/* ---- platform ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_PLATFORM_C

/* ---- entropy / RNG ---- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY  /* we feed OS entropy via entropy_poll */

/* ---- big integers & elliptic curves (ECDHE + ECDSA) ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C          /* ECDSA signature encoding */
/* curves AWS negotiates */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* ---- RSA (server certs & RSA key exchange fallback) ---- */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21             /* RSASSA-PSS in modern certs */
#define MBEDTLS_GENPRIME              /* pulled by rsa; harmless */

/* ---- hashes ---- */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C                /* some intermediate certs still sign w/ SHA1 */

/* ---- symmetric AEAD (AES-GCM only) ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
/* hardware AES: AES-NI on x86_64, ARMv8-A crypto ext on Graviton (arch-guarded,
   safe to define for either Lambda target) */
#define MBEDTLS_AESNI_C
#define MBEDTLS_AESCE_C

/* ---- public key plumbing ---- */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_OID_C

/* ---- X.509 chain verification (parse only) ---- */
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- TLS 1.2 client ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   /* SNI — required by AWS vhosts */
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* ---- sockets ---- */
#define MBEDTLS_NET_C

#endif /* CMM_MBEDTLS_CONFIG_H */
