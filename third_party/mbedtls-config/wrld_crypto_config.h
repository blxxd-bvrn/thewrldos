/* TF-PSA-Crypto configuration for THE WRLD OS: what a TLS 1.2/1.3 client needs,
 * no file system, no threads, entropy from the kernel. */
#ifndef PSA_CRYPTO_CONFIG_H
#define PSA_CRYPTO_CONFIG_H
#define TF_PSA_CRYPTO_CONFIG_VERSION 0x01000000

/* algorithms */
#define PSA_WANT_ALG_CHACHA20_POLY1305 1
#define PSA_WANT_ALG_GCM 1
#define PSA_WANT_ALG_ECDH 1
#define PSA_WANT_ALG_ECDSA 1
#define PSA_WANT_ALG_HKDF 1
#define PSA_WANT_ALG_HKDF_EXTRACT 1
#define PSA_WANT_ALG_HKDF_EXPAND 1
#define PSA_WANT_ALG_HMAC 1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN 1
#define PSA_WANT_ALG_RSA_PSS 1
#define PSA_WANT_ALG_RSA_PKCS1V15_CRYPT 1       /* required by the RSA certificate code paths */
#define PSA_WANT_ALG_RSA_OAEP 1
#define PSA_WANT_ALG_SHA_1 1                 /* signatures on some older roots/intermediates */
#define PSA_WANT_ALG_SHA_224 1
#define PSA_WANT_ALG_SHA_256 1
#define PSA_WANT_ALG_SHA_384 1
#define PSA_WANT_ALG_SHA_512 1
#define PSA_WANT_ALG_TLS12_PRF 1

/* curves: X25519, P-256, P-384, P-521 */
#define PSA_WANT_ECC_MONTGOMERY_255 1
#define PSA_WANT_ECC_SECP_R1_256 1
#define PSA_WANT_ECC_SECP_R1_384 1
#define PSA_WANT_ECC_SECP_R1_521 1

/* key types */
#define PSA_WANT_KEY_TYPE_DERIVE 1
#define PSA_WANT_KEY_TYPE_HMAC 1
#define PSA_WANT_KEY_TYPE_AES 1
#define PSA_WANT_KEY_TYPE_CHACHA20 1
#define PSA_WANT_KEY_TYPE_RAW_DATA 1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_EXPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_DERIVE 1

/* platform: the kernel's C library shim; time from the RTC */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_MS_TIME_ALT          /* mbedtls_ms_time() is in src/tls/tls.c */
#define MBEDTLS_PLATFORM_GMTIME_R_ALT         /* mbedtls_platform_gmtime_r() too */

/* modules */
#define MBEDTLS_MD_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C                   /* .pem / .crt files in C:\WRLD\Certificates */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY        /* mbedtls_platform_get_entropy() is in src/tls/tls.c */
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC

/* implementation choices */
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_UDBL_DIVISION              /* no __udivti3 in the x86-64 kernel */
#endif
