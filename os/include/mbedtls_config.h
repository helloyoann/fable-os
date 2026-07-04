/* mbedtls_config.h — mbedTLS 2.28 config for talk-os.
 *
 * Tuned for one job: a TLS 1.2 *client* that can complete a handshake with a
 * modern HTTPS server (ECDHE + AES-GCM / ChaCha20, P-256/P-384/X25519) from a
 * freestanding kernel. Selected with -DMBEDTLS_CONFIG_FILE="mbedtls_config.h".
 *
 * Deliberately minimal for a bare-metal target:
 *   - No filesystem, no sockets, no wall-clock time (no RTC in the kernel).
 *   - Entropy comes from mbedtls_hardware_poll() (RDRAND/TSC) — see libc_shim.c.
 *   - Certificate verification is left to the caller; talk-os runs with
 *     MBEDTLS_SSL_VERIFY_NONE (see lwipopts.h / README), so the cert is parsed
 *     but not trust-checked. Encrypted, but not authenticated.
 */
#ifndef TALKOS_MBEDTLS_CONFIG_H
#define TALKOS_MBEDTLS_CONFIG_H

/* ---- platform ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME                  /* uses time() below; NOT HAVE_TIME_DATE,
                                              so no certificate date validation */
#define MBEDTLS_NO_PLATFORM_ENTROPY        /* no /dev/urandom; use hardware poll */
#define MBEDTLS_ENTROPY_HARDWARE_ALT       /* we provide mbedtls_hardware_poll() */

/* ---- big numbers / elliptic curves / RSA ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_GENPRIME

/* ---- hashes ---- */
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C                      /* legacy cert signatures */
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* ---- symmetric ciphers / AEAD ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

/* ---- RNG ---- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* ---- ASN.1 / OID / public keys / X.509 ---- */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- TLS (client, 1.2) ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* ---- key exchanges (ECDHE preferred; plain RSA as a fallback) ---- */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

#include "mbedtls/check_config.h"

#endif /* TALKOS_MBEDTLS_CONFIG_H */
