/*
 * pki.c
 * This file is part of the SSH Library
 *
 * Copyright (c) 2010 by Aris Adamantiadis
 * Copyright (c) 2011-2013 Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2019      Sahana Prasad     <sahana@redhat.com>
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

/**
 * @defgroup libssh_pki The SSH Public Key Infrastructure
 * @ingroup libssh
 *
 * Functions for the creation, importation and manipulation of public and
 * private keys in the context of the SSH protocol
 *
 * @{
 */

#include "config.h"
#include "libssh/wrapper.h"

#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
# ifdef HAVE_IO_H
#  include <io.h>
#  undef open
#  define open _open
#  undef close
#  define close _close
#  undef read
#  define read _read
#  undef unlink
#  define unlink _unlink
# endif /* HAVE_IO_H */
#endif

#include "libssh/libssh.h"
#include "libssh/session.h"
#include "libssh/priv.h"
#include "libssh/pki.h"
#include "libssh/pki_priv.h"
#include "libssh/keys.h"
#include "libssh/buffer.h"
#include "libssh/misc.h"
#include "libssh/agent.h"

#ifndef MAX_LINE_SIZE
#define MAX_LINE_SIZE 4096
#endif

#define PKCS11_URI "pkcs11:"

enum ssh_keytypes_e pki_privatekey_type_from_string(const char *privkey)
{
    char *start = NULL;

    start = strstr(privkey, DSA_HEADER_BEGIN);
    if (start != NULL) {
        return SSH_KEYTYPE_DSS;
    }

    start = strstr(privkey, RSA_HEADER_BEGIN);
    if (start != NULL) {
        return SSH_KEYTYPE_RSA;
    }

    start = strstr(privkey, ECDSA_HEADER_BEGIN);
    if (start != 0) {
        /* We don't know what the curve is at this point, so we don't actually
         * know the type. We figure out the actual curve and fix things up in
         * pki_private_key_from_base64 */
        return SSH_KEYTYPE_ECDSA_P256;
    }

    return SSH_KEYTYPE_UNKNOWN;
}

/**
 * @brief returns the ECDSA key name ("ecdsa-sha2-nistp256" for example)
 *
 * @param[in] key the ssh_key whose ECDSA name to get
 *
 * @returns the ECDSA key name ("ecdsa-sha2-nistp256" for example)
 *
 * @returns "unknown" if the ECDSA key name is not known
 */
const char *ssh_pki_key_ecdsa_name(const ssh_key key)
{
    if (key == NULL) {
        return NULL;
    }

#ifdef HAVE_ECC /* FIXME Better ECC check needed */
    return pki_key_ecdsa_nid_to_name(key->ecdsa_nid);
#else
    return NULL;
#endif
}

/**
 * @brief creates a new empty SSH key
 * @returns an empty ssh_key handle, or NULL on error.
 */
ssh_key ssh_key_new (void) {
  ssh_key ptr = malloc (sizeof (struct ssh_key_struct));
  if (ptr == NULL) {
      return NULL;
  }
  ZERO_STRUCTP(ptr);
  return ptr;
}

ssh_key ssh_key_dup(const ssh_key key)
{
    if (key == NULL) {
        return NULL;
    }

    return pki_key_dup(key, 0);
}

/**
 * @brief clean up the key and deallocate all existing keys
 * @param[in] key ssh_key to clean
 */
void ssh_key_clean (ssh_key key){
    if(key == NULL)
        return;
#ifdef HAVE_LIBGCRYPT
    if(key->dsa) gcry_sexp_release(key->dsa);
    if(key->rsa) gcry_sexp_release(key->rsa);
    if(key->ecdsa) gcry_sexp_release(key->ecdsa);
#elif defined HAVE_LIBCRYPTO
    if(key->dsa) DSA_free(key->dsa);
    if(key->rsa) RSA_free(key->rsa);
#ifdef HAVE_OPENSSL_ECC
    if(key->ecdsa) EC_KEY_free(key->ecdsa);
#endif /* HAVE_OPENSSL_ECC */
#ifdef WITH_POST_QUANTUM_CRYPTO
    if (key->oqs_sk) {
        if (key->oqs_sig != NULL) {
            /* oqs_sig should always be non-NULL if oqs_sk is allocated. */
            explicit_bzero(key->oqs_sk, key->oqs_sig->length_secret_key);
        }
        SAFE_FREE(key->oqs_sk);
        key->oqs_sk = NULL;
    }
    if (key->oqs_pk) {
        SAFE_FREE(key->oqs_pk);
        key->oqs_pk = NULL;
    }
    if (key->oqs_sig) {
        OQS_SIG_free(key->oqs_sig);
        key->oqs_sig = NULL;
    }
#endif
#elif defined HAVE_LIBMBEDCRYPTO
    if (key->rsa != NULL) {
        mbedtls_pk_free(key->rsa);
        SAFE_FREE(key->rsa);
    }

    if (key->ecdsa != NULL) {
        mbedtls_ecdsa_free(key->ecdsa);
        SAFE_FREE(key->ecdsa);
    }
#endif
    if (key->ed25519_privkey != NULL){
#ifdef HAVE_OPENSSL_ED25519
        /* In OpenSSL implementation the private key is only the private
         * original seed. In the internal implementation the private key is the
         * concatenation of the original private seed with the public key.*/
        explicit_bzero(key->ed25519_privkey, ED25519_KEY_LEN);
#else
        explicit_bzero(key->ed25519_privkey, sizeof(ed25519_privkey));
#endif
        SAFE_FREE(key->ed25519_privkey);
    }
    SAFE_FREE(key->ed25519_pubkey);
    if (key->cert != NULL) {
        SSH_BUFFER_FREE(key->cert);
    }
    if (key->type == SSH_KEYTYPE_SK_ECDSA ||
        key->type == SSH_KEYTYPE_SK_ED25519 ||
        key->type == SSH_KEYTYPE_SK_ECDSA_CERT01 ||
        key->type == SSH_KEYTYPE_SK_ED25519_CERT01) {
        ssh_string_burn(key->sk_application);
        ssh_string_free(key->sk_application);
    }
    key->cert_type = SSH_KEYTYPE_UNKNOWN;
    key->flags=SSH_KEY_FLAG_EMPTY;
    key->type=SSH_KEYTYPE_UNKNOWN;
    key->ecdsa_nid = 0;
    key->type_c=NULL;
    key->dsa = NULL;
    key->rsa = NULL;
    key->ecdsa = NULL;
}

/**
 * @brief deallocate a SSH key
 * @param[in] key ssh_key handle to free
 */
void ssh_key_free (ssh_key key){
    if(key){
        ssh_key_clean(key);
        SAFE_FREE(key);
    }
}

/**
 * @brief returns the type of a ssh key
 * @param[in] key the ssh_key handle
 * @returns one of SSH_KEYTYPE_RSA, SSH_KEYTYPE_DSS,
 *          SSH_KEYTYPE_ECDSA_P256, SSH_KEYTYPE_ECDSA_P384,
 *          SSH_KEYTYPE_ECDSA_P521, SSH_KEYTYPE_ED25519, SSH_KEYTYPE_DSS_CERT01,
 *          SSH_KEYTYPE_RSA_CERT01, SSH_KEYTYPE_ECDSA_P256_CERT01,
 *          SSH_KEYTYPE_ECDSA_P384_CERT01, SSH_KEYTYPE_ECDSA_P521_CERT01, or
 *          SSH_KEYTYPE_ED25519_CERT01.
 * @returns SSH_KEYTYPE_UNKNOWN if the type is unknown
 */
enum ssh_keytypes_e ssh_key_type(const ssh_key key){
    if (key == NULL) {
        return SSH_KEYTYPE_UNKNOWN;
    }
    return key->type;
}

/**
 * @brief Convert a signature type to a string.
 *
 * @param[in]  type     The algorithm type to convert.
 *
 * @return              A string for the keytype or NULL if unknown.
 */
const char *
ssh_key_signature_to_char(enum ssh_keytypes_e type,
                          enum ssh_digest_e hash_type)
{
    switch (type) {
    case SSH_KEYTYPE_RSA:
        switch (hash_type) {
        case SSH_DIGEST_SHA256:
            return "rsa-sha2-256";
        case SSH_DIGEST_SHA512:
            return "rsa-sha2-512";
        case SSH_DIGEST_SHA1:
        case SSH_DIGEST_AUTO:
            return "ssh-rsa";
        default:
            return NULL;
        }
        break;
    case SSH_KEYTYPE_RSA_CERT01:
        switch (hash_type) {
        case SSH_DIGEST_SHA256:
            return "rsa-sha2-256-cert-v01@openssh.com";
        case SSH_DIGEST_SHA512:
            return "rsa-sha2-512-cert-v01@openssh.com";
        case SSH_DIGEST_SHA1:
        case SSH_DIGEST_AUTO:
            return "ssh-rsa-cert-v01@openssh.com";
        default:
            return NULL;
        }
        break;
    default:
        return ssh_key_type_to_char(type);
    }

    /* We should never reach this */
    return NULL;
}

/**
 * @brief Convert a key type to a string.
 *
 * @param[in]  type     The type to convert.
 *
 * @return              A string for the keytype or NULL if unknown.
 */
const char *ssh_key_type_to_char(enum ssh_keytypes_e type) {
  switch (type) {
    case SSH_KEYTYPE_DSS:
      return "ssh-dss";
    case SSH_KEYTYPE_RSA:
      return "ssh-rsa";
    case SSH_KEYTYPE_ECDSA:
      return "ssh-ecdsa"; /* deprecated. invalid value */
    case SSH_KEYTYPE_ECDSA_P256:
      return "ecdsa-sha2-nistp256";
    case SSH_KEYTYPE_ECDSA_P384:
      return "ecdsa-sha2-nistp384";
    case SSH_KEYTYPE_ECDSA_P521:
      return "ecdsa-sha2-nistp521";
    case SSH_KEYTYPE_ED25519:
      return "ssh-ed25519";
    case SSH_KEYTYPE_DSS_CERT01:
      return "ssh-dss-cert-v01@openssh.com";
    case SSH_KEYTYPE_RSA_CERT01:
      return "ssh-rsa-cert-v01@openssh.com";
    case SSH_KEYTYPE_ECDSA_P256_CERT01:
      return "ecdsa-sha2-nistp256-cert-v01@openssh.com";
    case SSH_KEYTYPE_ECDSA_P384_CERT01:
      return "ecdsa-sha2-nistp384-cert-v01@openssh.com";
    case SSH_KEYTYPE_ECDSA_P521_CERT01:
      return "ecdsa-sha2-nistp521-cert-v01@openssh.com";
    case SSH_KEYTYPE_ED25519_CERT01:
      return "ssh-ed25519-cert-v01@openssh.com";
    case SSH_KEYTYPE_SK_ECDSA:
      return "sk-ecdsa-sha2-nistp256@openssh.com";
    case SSH_KEYTYPE_SK_ED25519:
      return "sk-ssh-ed25519@openssh.com";
    case SSH_KEYTYPE_SK_ECDSA_CERT01:
      return "sk-ecdsa-sha2-nistp256-cert-v01@openssh.com";
    case SSH_KEYTYPE_SK_ED25519_CERT01:
      return "sk-ssh-ed25519-cert-v01@openssh.com";
#ifdef WITH_POST_QUANTUM_CRYPTO
    /* OQS-OpenSSH source: See keytypes array in sshkey.c:130 for this translation. */
///// OQS_TEMPLATE_FRAGMENT_ADD_PQ_KT_START
    case SSH_KEYTYPE_DILITHIUM_2:
      return "ssh-dilithium2";
    case SSH_KEYTYPE_FALCON_512:
      return "ssh-falcon512";
    case SSH_KEYTYPE_PICNIC_L1FULL:
      return "ssh-picnicl1full";
    case SSH_KEYTYPE_PICNIC3_L1:
      return "ssh-picnic3l1";
    case SSH_KEYTYPE_SPHINCS_HARAKA_128F_ROBUST:
      return "ssh-sphincsharaka128frobust";
    case SSH_KEYTYPE_SPHINCS_SHA256_128F_ROBUST:
      return "ssh-sphincssha256128frobust";
    case SSH_KEYTYPE_SPHINCS_SHAKE256_128F_ROBUST:
      return "ssh-sphincsshake256128frobust";
///// OQS_TEMPLATE_FRAGMENT_ADD_PQ_KT_END
///// OQS_TEMPLATE_FRAGMENT_ADD_HYBRID_KT_START
    case SSH_KEYTYPE_RSA3072_DILITHIUM_2:
      return "ssh-rsa3072-dilithium2";
    case SSH_KEYTYPE_P256_DILITHIUM_2:
      return "ssh-p256-dilithium2";
    case SSH_KEYTYPE_RSA3072_FALCON_512:
      return "ssh-rsa3072-falcon512";
    case SSH_KEYTYPE_P256_FALCON_512:
      return "ssh-p256-falcon512";
    case SSH_KEYTYPE_RSA3072_PICNIC_L1FULL:
      return "ssh-rsa3072-picnicl1full";
    case SSH_KEYTYPE_P256_PICNIC_L1FULL:
      return "ssh-p256-picnicl1full";
    case SSH_KEYTYPE_RSA3072_PICNIC3_L1:
      return "ssh-rsa3072-picnic3l1";
    case SSH_KEYTYPE_P256_PICNIC3_L1:
      return "ssh-p256-picnic3l1";
    case SSH_KEYTYPE_RSA3072_SPHINCS_HARAKA_128F_ROBUST:
      return "ssh-rsa3072-sphincsharaka128frobust";
    case SSH_KEYTYPE_P256_SPHINCS_HARAKA_128F_ROBUST:
      return "ssh-p256-sphincsharaka128frobust";
    case SSH_KEYTYPE_RSA3072_SPHINCS_SHA256_128F_ROBUST:
      return "ssh-rsa3072-sphincssha256128frobust";
    case SSH_KEYTYPE_P256_SPHINCS_SHA256_128F_ROBUST:
      return "ssh-p256-sphincssha256128frobust";
    case SSH_KEYTYPE_RSA3072_SPHINCS_SHAKE256_128F_ROBUST:
      return "ssh-rsa3072-sphincsshake256128frobust";
    case SSH_KEYTYPE_P256_SPHINCS_SHAKE256_128F_ROBUST:
      return "ssh-p256-sphincsshake256128frobust";
///// OQS_TEMPLATE_FRAGMENT_ADD_HYBRID_KT_END
#endif /* WITH_POST_QUANTUM_CRYPTO */
    case SSH_KEYTYPE_RSA1:
    case SSH_KEYTYPE_UNKNOWN:
      return NULL;
  }

  /* We should never reach this */
  return NULL;
}

enum ssh_digest_e ssh_key_hash_from_name(const char *name)
{
    if (name == NULL) {
        /* TODO we should rather fail */
        return SSH_DIGEST_AUTO;
    }

    if (strcmp(name, "ssh-rsa") == 0) {
        return SSH_DIGEST_SHA1;
    } else if (strcmp(name, "ssh-dss") == 0) {
        return SSH_DIGEST_SHA1;
    } else if (strcmp(name, "rsa-sha2-256") == 0) {
        return SSH_DIGEST_SHA256;
    } else if (strcmp(name, "rsa-sha2-512") == 0) {
        return SSH_DIGEST_SHA512;
    } else if (strcmp(name, "ecdsa-sha2-nistp256") == 0) {
        return SSH_DIGEST_SHA256;
    } else if (strcmp(name, "ecdsa-sha2-nistp384") == 0) {
        return SSH_DIGEST_SHA384;
    } else if (strcmp(name, "ecdsa-sha2-nistp521") == 0) {
        return SSH_DIGEST_SHA512;
    } else if (strcmp(name, "ssh-ed25519") == 0) {
        return SSH_DIGEST_AUTO;
    } else if (strcmp(name, "sk-ecdsa-sha2-nistp256@openssh.com") == 0) {
        return SSH_DIGEST_SHA256;
    } else if (strcmp(name, "sk-ssh-ed25519@openssh.com") == 0) {
        return SSH_DIGEST_AUTO;
    }

#ifdef WITH_POST_QUANTUM_CRYPTO
///// OQS_TEMPLATE_FRAGMENT_ADD_PQ_KEY_HASH_START
    if (strcmp(name, "ssh-dilithium2") == 0) {
        return SSH_DIGEST_AUTO;
    }
    if (strcmp(name, "ssh-falcon512") == 0) {
        return SSH_DIGEST_AUTO;
    }
    if (strcmp(name, "ssh-picnicl1full") == 0) {
        return SSH_DIGEST_AUTO;
    }
    if (strcmp(name, "ssh-picnic3l1") == 0) {
        return SSH_DIGEST_AUTO;
    }
    if (strcmp(name, "ssh-sphincsharaka128frobust") == 0) {
        return SSH_DIGEST_AUTO;
    }
    if (strcmp(name, "ssh-sphincssha256128frobust") == 0) {
        return SSH_DIGEST_AUTO;
    }
    if (strcmp(name, "ssh-sphincsshake256128frobust") == 0) {
        return SSH_DIGEST_AUTO;
    }
///// OQS_TEMPLATE_FRAGMENT_ADD_PQ_KEY_HASH_END
///// OQS_TEMPLATE_FRAGMENT_ADD_HYBRID_KEY_HASH_START
    if (strcmp(name, "ssh-rsa3072-dilithium2") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-rsa3072-falcon512") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-rsa3072-picnicl1full") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-rsa3072-picnic3l1") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-rsa3072-sphincsharaka128frobust") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-rsa3072-sphincssha256128frobust") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-rsa3072-sphincsshake256128frobust") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-p256-dilithium2") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-p256-falcon512") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-p256-picnicl1full") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-p256-picnic3l1") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-p256-sphincsharaka128frobust") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-p256-sphincssha256128frobust") == 0) {
        return SSH_DIGEST_SHA256;
    }
    if (strcmp(name, "ssh-p256-sphincsshake256128frobust") == 0) {
        return SSH_DIGEST_SHA256;
    }
///// OQS_TEMPLATE_FRAGMENT_ADD_HYBRID_KEY_HASH_END
#endif

    SSH_LOG(SSH_LOG_WARN, "Unknown signature name %s", name);

    /* TODO we should rather fail */
    return SSH_DIGEST_AUTO;
}

/**
 * @brief Checks the given key against the configured allowed
 * public key algorithm types
 *
 * @param[in] session The SSH session
 * @param[in] type    The key algorithm to check
 * @returns           1 if the key algorithm is allowed, 0 otherwise
 */
int ssh_key_algorithm_allowed(ssh_session session, const char *type)
{
    const char *allowed_list;

    if (session->client) {
        allowed_list = session->opts.pubkey_accepted_types;
        if (allowed_list == NULL) {
            if (ssh_fips_mode()) {
                allowed_list = ssh_kex_get_fips_methods(SSH_HOSTKEYS);
            } else {
                allowed_list = ssh_kex_get_default_methods(SSH_HOSTKEYS);
            }
        }
    }
#ifdef WITH_SERVER
    else if (session->server) {
        allowed_list = session->opts.wanted_methods[SSH_HOSTKEYS];
        if (allowed_list == NULL) {
            SSH_LOG(SSH_LOG_WARN, "Session invalid: no host key available");
            return 0;
        }
    }
#endif
    else {
        SSH_LOG(SSH_LOG_WARN, "Session invalid: not set as client nor server");
        return 0;
    }

    SSH_LOG(SSH_LOG_DEBUG, "Checking %s with list <%s>", type, allowed_list);
    return ssh_match_group(allowed_list, type);
}

/**
 * @brief Convert a key type to a hash type. This is usually unambiguous
 * for all the key types, unless the SHA2 extension (RFC 8332) is
 * negotiated during key exchange.
 *
 * @param[in]  session  SSH Session.
 *
 * @param[in]  type     The type to convert.
 *
 * @return              A hash type to be used.
 */
enum ssh_digest_e ssh_key_type_to_hash(ssh_session session,
                                       enum ssh_keytypes_e type)
{
    switch (type) {
    case SSH_KEYTYPE_DSS_CERT01:
    case SSH_KEYTYPE_DSS:
        return SSH_DIGEST_SHA1;
    case SSH_KEYTYPE_RSA_CERT01:
        /* If we are talking to an old OpenSSH version which does not support
         * SHA2 in certificates */
        if ((session->openssh > 0) &&
            (session->openssh < SSH_VERSION_INT(7, 2, 0)))
        {
            SSH_LOG(SSH_LOG_DEBUG,
                    "We are talking to an old OpenSSH (%x); "
                    "returning SSH_DIGEST_SHA1",
                    session->openssh);

            return SSH_DIGEST_SHA1;
        }
        FALL_THROUGH;
    case SSH_KEYTYPE_RSA:
        if (ssh_key_algorithm_allowed(session, "rsa-sha2-512") &&
            (session->extensions & SSH_EXT_SIG_RSA_SHA512)) {
            return SSH_DIGEST_SHA512;
        }

        if (ssh_key_algorithm_allowed(session, "rsa-sha2-256") &&
            (session->extensions & SSH_EXT_SIG_RSA_SHA256)) {
            return SSH_DIGEST_SHA256;
        }

        /* Default algorithm for RSA is SHA1 */
        return SSH_DIGEST_SHA1;

    case SSH_KEYTYPE_ECDSA_P256_CERT01:
    case SSH_KEYTYPE_ECDSA_P256:
        return SSH_DIGEST_SHA256;
    case SSH_KEYTYPE_ECDSA_P384_CERT01:
    case SSH_KEYTYPE_ECDSA_P384:
        return SSH_DIGEST_SHA384;
    case SSH_KEYTYPE_ECDSA_P521_CERT01:
    case SSH_KEYTYPE_ECDSA_P521:
        return SSH_DIGEST_SHA512;
    case SSH_KEYTYPE_ED25519_CERT01:
    case SSH_KEYTYPE_ED25519:
        return SSH_DIGEST_AUTO;
#ifdef WITH_POST_QUANTUM_CRYPTO
    CASE_KEY_OQS:
       return SSH_DIGEST_AUTO;
///// OQS_TEMPLATE_FRAGMENT_HYBRID_KT_TO_HASH_START
    CASE_KEY_RSA_HYBRID:
    case SSH_KEYTYPE_P256_DILITHIUM_2:
    case SSH_KEYTYPE_P256_FALCON_512:
    case SSH_KEYTYPE_P256_PICNIC_L1FULL:
    case SSH_KEYTYPE_P256_PICNIC3_L1:
    case SSH_KEYTYPE_P256_SPHINCS_HARAKA_128F_ROBUST:
    case SSH_KEYTYPE_P256_SPHINCS_SHA256_128F_ROBUST:
    case SSH_KEYTYPE_P256_SPHINCS_SHAKE256_128F_ROBUST:

        return SSH_DIGEST_SHA256;
///// OQS_TEMPLATE_FRAGMENT_HYBRID_KT_TO_HASH_END
#endif
    case SSH_KEYTYPE_RSA1:
    case SSH_KEYTYPE_ECDSA:
    case SSH_KEYTYPE_UNKNOWN:
    default:
        SSH_LOG(SSH_LOG_WARN, "Digest algorithm to be used with key type %u "
                "is not defined", type);
    }

    /* We should never reach this */
    return SSH_DIGEST_AUTO;
}

/**
 * @brief Gets signature algorithm name to be used with the given
 *        key type.
 *
 * @param[in]  session  SSH session.
 * @param[in]  type     The algorithm type to convert.
 *
 * @return              A string for the keytype or NULL if unknown.
 */
const char *
ssh_key_get_signature_algorithm(ssh_session session,
                                enum ssh_keytypes_e type)
{
    enum ssh_digest_e hash_type;

    if (type == SSH_KEYTYPE_RSA_CERT01) {
        /* If we are talking to an old OpenSSH version which does not support
         * rsa-sha2-{256,512}-cert-v01@openssh.com */
        if ((session->openssh > 0) &&
            (session->openssh < SSH_VERSION_INT(7, 8, 0)))
        {
            SSH_LOG(SSH_LOG_DEBUG,
                    "We are talking to an old OpenSSH (%x); "
                    "using old cert format",
                    session->openssh);

            return "ssh-rsa-cert-v01@openssh.com";
        }
    }

    hash_type = ssh_key_type_to_hash(session, type);

    return ssh_key_signature_to_char(type, hash_type);
}

/**
 * @brief Convert a ssh key algorithm name to a ssh key algorithm type.
 *
 * @param[in] name      The name to convert.
 *
 * @return              The enum ssh key algorithm type.
 */
enum ssh_keytypes_e ssh_key_type_from_signature_name(const char *name) {
    if (name == NULL) {
        return SSH_KEYTYPE_UNKNOWN;
    }

    if ((strcmp(name, "rsa-sha2-256") == 0) ||
        (strcmp(name, "rsa-sha2-512") == 0)) {
        return SSH_KEYTYPE_RSA;
    }

    /* Otherwise the key type matches the signature type */
    return ssh_key_type_from_name(name);
}

/**
 * @brief Convert a ssh key name to a ssh key type.
 *
 * @param[in] name      The name to convert.
 *
 * @return              The enum ssh key type.
 */
enum ssh_keytypes_e ssh_key_type_from_name(const char *name) {
    if (name == NULL) {
        return SSH_KEYTYPE_UNKNOWN;
    }

    if (strcmp(name, "rsa") == 0) {
        return SSH_KEYTYPE_RSA;
    } else if (strcmp(name, "dsa") == 0) {
        return SSH_KEYTYPE_DSS;
    } else if (strcmp(name, "ssh-rsa") == 0) {
        return SSH_KEYTYPE_RSA;
    } else if (strcmp(name, "ssh-dss") == 0) {
        return SSH_KEYTYPE_DSS;
    } else if (strcmp(name, "ssh-ecdsa") == 0
            || strcmp(name, "ecdsa") == 0
            || strcmp(name, "ecdsa-sha2-nistp256") == 0) {
        return SSH_KEYTYPE_ECDSA_P256;
    } else if (strcmp(name, "ecdsa-sha2-nistp384") == 0) {
        return SSH_KEYTYPE_ECDSA_P384;
    } else if (strcmp(name, "ecdsa-sha2-nistp521") == 0) {
        return SSH_KEYTYPE_ECDSA_P521;
    } else if (strcmp(name, "ssh-ed25519") == 0){
        return SSH_KEYTYPE_ED25519;
    } else if (strcmp(name, "ssh-dss-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_DSS_CERT01;
    } else if (strcmp(name, "ssh-rsa-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_RSA_CERT01;
    } else if (strcmp(name, "ecdsa-sha2-nistp256-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_ECDSA_P256_CERT01;
    } else if (strcmp(name, "ecdsa-sha2-nistp384-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_ECDSA_P384_CERT01;
    } else if (strcmp(name, "ecdsa-sha2-nistp521-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_ECDSA_P521_CERT01;
    } else if (strcmp(name, "ssh-ed25519-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_ED25519_CERT01;
    } else if(strcmp(name, "sk-ecdsa-sha2-nistp256@openssh.com") == 0) {
        return SSH_KEYTYPE_SK_ECDSA;
    } else if(strcmp(name, "sk-ecdsa-sha2-nistp256-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_SK_ECDSA_CERT01;
    } else if(strcmp(name, "sk-ssh-ed25519@openssh.com") == 0) {
        return SSH_KEYTYPE_SK_ED25519;
    } else if(strcmp(name, "sk-ssh-ed25519-cert-v01@openssh.com") == 0) {
        return SSH_KEYTYPE_SK_ED25519_CERT01;
    }

#ifdef WITH_POST_QUANTUM_CRYPTO
    /* OQS-OpenSSH source: See keytypes array in sshkey.c:130 for this translation. */
///// OQS_TEMPLATE_FRAGMENT_ADD_PQ_KT_STRING_START
    if (strcmp(name, "ssh-dilithium2") == 0) {
        return SSH_KEYTYPE_DILITHIUM_2;
    }
    if (strcmp(name, "ssh-falcon512") == 0) {
        return SSH_KEYTYPE_FALCON_512;
    }
    if (strcmp(name, "ssh-picnicl1full") == 0) {
        return SSH_KEYTYPE_PICNIC_L1FULL;
    }
    if (strcmp(name, "ssh-picnic3l1") == 0) {
        return SSH_KEYTYPE_PICNIC3_L1;
    }
    if (strcmp(name, "ssh-sphincsharaka128frobust") == 0) {
        return SSH_KEYTYPE_SPHINCS_HARAKA_128F_ROBUST;
    }
    if (strcmp(name, "ssh-sphincssha256128frobust") == 0) {
        return SSH_KEYTYPE_SPHINCS_SHA256_128F_ROBUST;
    }
    if (strcmp(name, "ssh-sphincsshake256128frobust") == 0) {
        return SSH_KEYTYPE_SPHINCS_SHAKE256_128F_ROBUST;
    }
///// OQS_TEMPLATE_FRAGMENT_ADD_PQ_KT_STRING_END
///// OQS_TEMPLATE_FRAGMENT_ADD_HYBRID_KT_STRING_START
    if (strcmp(name, "ssh-rsa3072-dilithium2") == 0) {
        return SSH_KEYTYPE_RSA3072_DILITHIUM_2;
    }
    if (strcmp(name, "ssh-p256-dilithium2") == 0) {
        return SSH_KEYTYPE_P256_DILITHIUM_2;
    }
    if (strcmp(name, "ssh-rsa3072-falcon512") == 0) {
        return SSH_KEYTYPE_RSA3072_FALCON_512;
    }
    if (strcmp(name, "ssh-p256-falcon512") == 0) {
        return SSH_KEYTYPE_P256_FALCON_512;
    }
    if (strcmp(name, "ssh-rsa3072-picnicl1full") == 0) {
        return SSH_KEYTYPE_RSA3072_PICNIC_L1FULL;
    }
    if (strcmp(name, "ssh-p256-picnicl1full") == 0) {
        return SSH_KEYTYPE_P256_PICNIC_L1FULL;
    }
    if (strcmp(name, "ssh-rsa3072-picnic3l1") == 0) {
        return SSH_KEYTYPE_RSA3072_PICNIC3_L1;
    }
    if (strcmp(name, "ssh-p256-picnic3l1") == 0) {
        return SSH_KEYTYPE_P256_PICNIC3_L1;
    }
    if (strcmp(name, "ssh-rsa3072-sphincsharaka128frobust") == 0) {
        return SSH_KEYTYPE_RSA3072_SPHINCS_HARAKA_128F_ROBUST;
    }
    if (strcmp(name, "ssh-p256-sphincsharaka128frobust") == 0) {
        return SSH_KEYTYPE_P256_SPHINCS_HARAKA_128F_ROBUST;
    }
    if (strcmp(name, "ssh-rsa3072-sphincssha256128frobust") == 0) {
        return SSH_KEYTYPE_RSA3072_SPHINCS_SHA256_128F_ROBUST;
    }
    if (strcmp(name, "ssh-p256-sphincssha256128frobust") == 0) {
        return SSH_KEYTYPE_P256_SPHINCS_SHA256_128F_ROBUST;
    }
    if (strcmp(name, "ssh-rsa3072-sphincsshake256128frobust") == 0) {
        return SSH_KEYTYPE_RSA3072_SPHINCS_SHAKE256_128F_ROBUST;
    }
    if (strcmp(name, "ssh-p256-sphincsshake256128frobust") == 0) {
        return SSH_KEYTYPE_P256_SPHINCS_SHAKE256_128F_ROBUST;
    }
///// OQS_TEMPLATE_FRAGMENT_ADD_HYBRID_KT_STRING_END
#endif

    return SSH_KEYTYPE_UNKNOWN;
}

/**
 * @brief Get the pubic key type corresponding to a certificate type.
 *
 * @param[in] type   The certificate or public key type.
 *
 * @return           The matching public key type.
 */
enum ssh_keytypes_e ssh_key_type_plain(enum ssh_keytypes_e type) {
    switch (type) {
        case SSH_KEYTYPE_DSS_CERT01:
            return SSH_KEYTYPE_DSS;
        case SSH_KEYTYPE_RSA_CERT01:
            return SSH_KEYTYPE_RSA;
        case SSH_KEYTYPE_ECDSA_P256_CERT01:
            return SSH_KEYTYPE_ECDSA_P256;
        case SSH_KEYTYPE_ECDSA_P384_CERT01:
            return SSH_KEYTYPE_ECDSA_P384;
        case SSH_KEYTYPE_ECDSA_P521_CERT01:
            return SSH_KEYTYPE_ECDSA_P521;
        case SSH_KEYTYPE_ED25519_CERT01:
            return SSH_KEYTYPE_ED25519;
        case SSH_KEYTYPE_SK_ECDSA_CERT01:
            return SSH_KEYTYPE_SK_ECDSA;
        case SSH_KEYTYPE_SK_ED25519_CERT01:
            return SSH_KEYTYPE_SK_ED25519;
        default:
            return type;
    }
}

/**
 * @brief Check if the key has/is a public key.
 *
 * @param[in] k         The key to check.
 *
 * @return              1 if it is a public key, 0 if not.
 */
int ssh_key_is_public(const ssh_key k) {
    if (k == NULL) {
        return 0;
    }

    return (k->flags & SSH_KEY_FLAG_PUBLIC) == SSH_KEY_FLAG_PUBLIC;
}

/**
 * @brief Check if the key is a private key.
 *
 * @param[in] k         The key to check.
 *
 * @return              1 if it is a private key, 0 if not.
 */
int ssh_key_is_private(const ssh_key k) {
    if (k == NULL) {
        return 0;
    }

    return (k->flags & SSH_KEY_FLAG_PRIVATE) == SSH_KEY_FLAG_PRIVATE;
}

/**
 * @brief Compare keys if they are equal.
 *
 * @param[in] k1        The first key to compare.
 *
 * @param[in] k2        The second key to compare.
 *
 * @param[in] what      What part or type of the key do you want to compare.
 *
 * @return              0 if equal, 1 if not.
 */
int ssh_key_cmp(const ssh_key k1,
                const ssh_key k2,
                enum ssh_keycmp_e what)
{
    if (k1 == NULL || k2 == NULL) {
        return 1;
    }

    if (k1->type != k2->type) {
        SSH_LOG(SSH_LOG_WARN, "key types don't match!");
        return 1;
    }

    if (what == SSH_KEY_CMP_PRIVATE) {
        if (!ssh_key_is_private(k1) ||
            !ssh_key_is_private(k2)) {
            return 1;
        }
    }

    if (k1->type == SSH_KEYTYPE_SK_ECDSA ||
        k1->type == SSH_KEYTYPE_SK_ED25519) {
        if (strncmp(ssh_string_get_char(k1->sk_application),
                ssh_string_get_char(k2->sk_application),
                ssh_string_len(k2->sk_application)) != 0) {
            return 1;
        }
    }

    if (k1->type == SSH_KEYTYPE_ED25519 ||
        k1->type == SSH_KEYTYPE_SK_ED25519) {
        return pki_ed25519_key_cmp(k1, k2, what);
    }

    return pki_key_compare(k1, k2, what);
}

ssh_signature ssh_signature_new(void)
{
    struct ssh_signature_struct *sig;

    sig = malloc(sizeof(struct ssh_signature_struct));
    if (sig == NULL) {
        return NULL;
    }
    ZERO_STRUCTP(sig);

    return sig;
}

void ssh_signature_free(ssh_signature sig)
{
    if (sig == NULL) {
        return;
    }

    switch(sig->type) {
        case SSH_KEYTYPE_DSS:
#ifdef HAVE_LIBGCRYPT
            gcry_sexp_release(sig->dsa_sig);
#endif
            break;
        case SSH_KEYTYPE_RSA:
#ifdef HAVE_LIBGCRYPT
            gcry_sexp_release(sig->rsa_sig);
#elif defined HAVE_LIBMBEDCRYPTO
            SAFE_FREE(sig->rsa_sig);
#endif
            break;
        case SSH_KEYTYPE_ECDSA_P256:
        case SSH_KEYTYPE_ECDSA_P384:
        case SSH_KEYTYPE_ECDSA_P521:
        case SSH_KEYTYPE_SK_ECDSA:
#ifdef HAVE_GCRYPT_ECC
            gcry_sexp_release(sig->ecdsa_sig);
#elif defined HAVE_LIBMBEDCRYPTO
            bignum_safe_free(sig->ecdsa_sig.r);
            bignum_safe_free(sig->ecdsa_sig.s);
#endif
            break;
        case SSH_KEYTYPE_ED25519:
        case SSH_KEYTYPE_SK_ED25519:
#ifndef HAVE_OPENSSL_ED25519
            /* When using OpenSSL, the signature is stored in sig->raw_sig */
            SAFE_FREE(sig->ed25519_sig);
#endif
            break;
#ifdef WITH_POST_QUANTUM_CRYPTO
        CASE_KEY_OQS:
        CASE_KEY_HYBRID:
            ssh_string_burn(sig->pq_sig);
            SSH_STRING_FREE(sig->pq_sig);
            break;
#endif
        case SSH_KEYTYPE_DSS_CERT01:
        case SSH_KEYTYPE_RSA_CERT01:
        case SSH_KEYTYPE_ECDSA_P256_CERT01:
        case SSH_KEYTYPE_ECDSA_P384_CERT01:
        case SSH_KEYTYPE_ECDSA_P521_CERT01:
        case SSH_KEYTYPE_ED25519_CERT01:
        case SSH_KEYTYPE_SK_ECDSA_CERT01:
        case SSH_KEYTYPE_SK_ED25519_CERT01:
        case SSH_KEYTYPE_RSA1:
        case SSH_KEYTYPE_ECDSA:
        case SSH_KEYTYPE_UNKNOWN:
            break;
    }

    /* Explicitly zero the signature content before free */
    ssh_string_burn(sig->raw_sig);
    SSH_STRING_FREE(sig->raw_sig);
    SAFE_FREE(sig);
}

/**
 * @brief import a base64 formated key from a memory c-string
 *
 * @param[in]  b64_key  The c-string holding the base64 encoded key
 *
 * @param[in]  passphrase The passphrase to decrypt the key, or NULL
 *
 * @param[in]  auth_fn  An auth function you may want to use or NULL.
 *
 * @param[in]  auth_data Private data passed to the auth function.
 *
 * @param[out] pkey     A pointer where the allocated key can be stored. You
 *                      need to free the memory.
 *
 * @return  SSH_ERROR in case of error, SSH_OK otherwise.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_privkey_base64(const char *b64_key,
                                  const char *passphrase,
                                  ssh_auth_callback auth_fn,
                                  void *auth_data,
                                  ssh_key *pkey)
{
    ssh_key key;
    char *openssh_header = NULL;

    if (b64_key == NULL || pkey == NULL) {
        return SSH_ERROR;
    }

    if (b64_key == NULL || !*b64_key) {
        return SSH_ERROR;
    }

    SSH_LOG(SSH_LOG_INFO,
            "Trying to decode privkey passphrase=%s",
            passphrase ? "true" : "false");

    /* Test for OpenSSH key format first */
    openssh_header = strstr(b64_key, OPENSSH_HEADER_BEGIN);
    if (openssh_header != NULL) {
        key = ssh_pki_openssh_privkey_import(openssh_header,
                                             passphrase,
                                             auth_fn,
                                             auth_data);
    } else {
        /* fallback on PEM decoder */
        key = pki_private_key_from_base64(b64_key,
                                          passphrase,
                                          auth_fn,
                                          auth_data);
    }
    if (key == NULL) {
        return SSH_ERROR;
    }

    *pkey = key;

    return SSH_OK;
}
 /**
 * @brief Convert a private key to a pem base64 encoded key, or OpenSSH format for
 *        keytype ssh-ed25519
 *
 * @param[in]  privkey  The private key to export.
 *
 * @param[in]  passphrase The passphrase to use to encrypt the key with or
 *             NULL. An empty string means no passphrase.
 *
 * @param[in]  auth_fn  An auth function you may want to use or NULL.
 *
 * @param[in]  auth_data Private data passed to the auth function.
 *
 * @param[out] b64_key  A pointer to store the allocated base64 encoded key. You
 *                      need to free the buffer.
 *
 * @return     SSH_OK on success, SSH_ERROR on error.
 */
int ssh_pki_export_privkey_base64(const ssh_key privkey,
                                  const char *passphrase,
                                  ssh_auth_callback auth_fn,
                                  void *auth_data,
                                  char **b64_key)
{
    ssh_string blob = NULL;
    char *b64 = NULL;

    if (privkey == NULL || !ssh_key_is_private(privkey)) {
        return SSH_ERROR;
    }

    if (privkey->type == SSH_KEYTYPE_ED25519){
        blob = ssh_pki_openssh_privkey_export(privkey,
                                              passphrase,
                                              auth_fn,
                                              auth_data);
    } else {
        blob = pki_private_key_to_pem(privkey,
                                      passphrase,
                                      auth_fn,
                                      auth_data);
    }
    if (blob == NULL) {
        return SSH_ERROR;
    }

    b64 = strndup(ssh_string_data(blob), ssh_string_len(blob));
    SSH_STRING_FREE(blob);
    if (b64 == NULL) {
        return SSH_ERROR;
    }

    *b64_key = b64;

    return SSH_OK;
}

/**
 * @brief Import a private key from a file or a PKCS #11 device.
 *
 * @param[in]  filename The filename of the private key or the
 *                      PKCS #11 URI corresponding to the private key.
 *
 * @param[in]  passphrase The passphrase to decrypt the private key. Set to NULL
 *                        if none is needed or it is unknown.
 *
 * @param[in]  auth_fn  An auth function you may want to use or NULL.
 *
 * @param[in]  auth_data Private data passed to the auth function.
 *
 * @param[out] pkey     A pointer to store the allocated ssh_key. You need to
 *                      free the key.
 *
 * @returns SSH_OK on success, SSH_EOF if the file doesn't exist or permission
 *          denied, SSH_ERROR otherwise.
 *
 * @see ssh_key_free()
 **/
int ssh_pki_import_privkey_file(const char *filename,
                                const char *passphrase,
                                ssh_auth_callback auth_fn,
                                void *auth_data,
                                ssh_key *pkey) {
    struct stat sb;
    char *key_buf;
    FILE *file;
    off_t size;
    int rc;

    if (pkey == NULL || filename == NULL || *filename == '\0') {
        return SSH_ERROR;
    }

#ifdef WITH_PKCS11_URI
    if (ssh_pki_is_uri(filename)) {
        rc = pki_uri_import(filename, pkey, SSH_KEY_PRIVATE);
        return rc;
    }
#endif

    file = fopen(filename, "rb");
    if (file == NULL) {
        SSH_LOG(SSH_LOG_WARN,
                "Error opening %s: %s",
                filename,
                strerror(errno));
        return SSH_EOF;
    }

    rc = fstat(fileno(file), &sb);
    if (rc < 0) {
        fclose(file);
        SSH_LOG(SSH_LOG_WARN,
                "Error getting stat of %s: %s",
                filename,
                strerror(errno));
        switch (errno) {
            case ENOENT:
            case EACCES:
                return SSH_EOF;
        }

        return SSH_ERROR;
    }

    if (sb.st_size > MAX_PRIVKEY_SIZE) {
#ifdef WITH_POST_QUANTUM_CRYPTO
        SSH_LOG(SSH_LOG_WARN,
                "Private key is bigger than 8M.");
#else
        SSH_LOG(SSH_LOG_WARN,
                "Private key is bigger than 4M.");
#endif
        fclose(file);
        return SSH_ERROR;
    }

    key_buf = malloc(sb.st_size + 1);
    if (key_buf == NULL) {
        fclose(file);
        SSH_LOG(SSH_LOG_WARN, "Out of memory!");
        return SSH_ERROR;
    }

    size = fread(key_buf, 1, sb.st_size, file);
    fclose(file);

    if (size != sb.st_size) {
        SAFE_FREE(key_buf);
        SSH_LOG(SSH_LOG_WARN,
                "Error reading %s: %s",
                filename,
                strerror(errno));
        return SSH_ERROR;
    }
    key_buf[size] = 0;

    rc = ssh_pki_import_privkey_base64(key_buf,
                                       passphrase,
                                       auth_fn,
                                       auth_data,
                                       pkey);

    SAFE_FREE(key_buf);
    return rc;
}

/**
 * @brief Export a private key to a pem file on disk, or OpenSSH format for
 *        keytype ssh-ed25519
 *
 * @param[in]  privkey  The private key to export.
 *
 * @param[in]  passphrase The passphrase to use to encrypt the key with or
 *             NULL. An empty string means no passphrase.
 *
 * @param[in]  auth_fn  An auth function you may want to use or NULL.
 *
 * @param[in]  auth_data Private data passed to the auth function.
 *
 * @param[in]  filename  The path where to store the pem file.
 *
 * @return     SSH_OK on success, SSH_ERROR on error.
 */
int ssh_pki_export_privkey_file(const ssh_key privkey,
                                const char *passphrase,
                                ssh_auth_callback auth_fn,
                                void *auth_data,
                                const char *filename)
{
    ssh_string blob;
    FILE *fp;
    int rc;

    if (privkey == NULL || !ssh_key_is_private(privkey)) {
        return SSH_ERROR;
    }

    fp = fopen(filename, "wb");
    if (fp == NULL) {
        SSH_LOG(SSH_LOG_FUNCTIONS, "Error opening %s: %s",
                filename, strerror(errno));
        return SSH_EOF;
    }

    if (privkey->type == SSH_KEYTYPE_ED25519){
        blob = ssh_pki_openssh_privkey_export(privkey,
                                              passphrase,
                                              auth_fn,
                                              auth_data);
    } else {
        blob = pki_private_key_to_pem(privkey,
                                      passphrase,
                                      auth_fn,
                                      auth_data);
    }
    if (blob == NULL) {
        fclose(fp);
        return -1;
    }

    rc = fwrite(ssh_string_data(blob), ssh_string_len(blob), 1, fp);
    SSH_STRING_FREE(blob);
    if (rc != 1 || ferror(fp)) {
        fclose(fp);
        unlink(filename);
        return SSH_ERROR;
    }
    fclose(fp);

    return SSH_OK;
}

/* temporary function to migrate seemlessly to ssh_key */
ssh_public_key ssh_pki_convert_key_to_publickey(const ssh_key key) {
    ssh_public_key pub;
    ssh_key tmp;

    if(key == NULL) {
        return NULL;
    }

    tmp = ssh_key_dup(key);
    if (tmp == NULL) {
        return NULL;
    }

    pub = malloc(sizeof(struct ssh_public_key_struct));
    if (pub == NULL) {
        ssh_key_free(tmp);
        return NULL;
    }
    ZERO_STRUCTP(pub);

    pub->type = tmp->type;
    pub->type_c = tmp->type_c;

    pub->dsa_pub = tmp->dsa;
    tmp->dsa = NULL;
    pub->rsa_pub = tmp->rsa;
    tmp->rsa = NULL;

    ssh_key_free(tmp);

    return pub;
}

ssh_private_key ssh_pki_convert_key_to_privatekey(const ssh_key key) {
    ssh_private_key privkey;

    privkey = malloc(sizeof(struct ssh_private_key_struct));
    if (privkey == NULL) {
        ssh_key_free(key);
        return NULL;
    }

    privkey->type = key->type;
    privkey->dsa_priv = key->dsa;
    privkey->rsa_priv = key->rsa;

    return privkey;
}

int pki_import_privkey_buffer(enum ssh_keytypes_e type,
                              ssh_buffer buffer,
                              ssh_key *pkey)
{
    ssh_key key = NULL;
    int rc;

    key = ssh_key_new();
    if (key == NULL) {
        return SSH_ERROR;
    }

    key->type = type;
    key->type_c = ssh_key_type_to_char(type);
    key->flags = SSH_KEY_FLAG_PRIVATE | SSH_KEY_FLAG_PUBLIC;

    switch (type) {
        case SSH_KEYTYPE_DSS:
            {
                ssh_string p = NULL;
                ssh_string q = NULL;
                ssh_string g = NULL;
                ssh_string pubkey = NULL;
                ssh_string privkey = NULL;

                rc = ssh_buffer_unpack(buffer, "SSSSS", &p, &q, &g,
                                       &pubkey, &privkey);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_WARN, "Unpack error");
                    goto fail;
                }

                rc = pki_privkey_build_dss(key, p, q, g, pubkey, privkey);
#ifdef DEBUG_CRYPTO
                ssh_log_hexdump("p", ssh_string_data(p), ssh_string_len(p));
                ssh_log_hexdump("q", ssh_string_data(q), ssh_string_len(q));
                ssh_log_hexdump("g", ssh_string_data(g), ssh_string_len(g));
                ssh_log_hexdump("pubkey", ssh_string_data(pubkey),
                               ssh_string_len(pubkey));
                ssh_log_hexdump("privkey", ssh_string_data(privkey),
                               ssh_string_len(privkey));
#endif
                ssh_string_burn(p);
                SSH_STRING_FREE(p);
                ssh_string_burn(q);
                SSH_STRING_FREE(q);
                ssh_string_burn(g);
                SSH_STRING_FREE(g);
                ssh_string_burn(pubkey);
                SSH_STRING_FREE(pubkey);
                ssh_string_burn(privkey);
                SSH_STRING_FREE(privkey);
                if (rc == SSH_ERROR) {
                    goto fail;
                }
            }
            break;
        case SSH_KEYTYPE_RSA:
#ifdef WITH_POST_QUANTUM_CRYPTO
        CASE_KEY_RSA_HYBRID:
#endif
            {
                ssh_string n = NULL;
                ssh_string e = NULL;
                ssh_string d = NULL;
                ssh_string iqmp = NULL;
                ssh_string p = NULL;
                ssh_string q = NULL;

                rc = ssh_buffer_unpack(buffer, "SSSSSS", &n, &e, &d,
                                       &iqmp, &p, &q);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_WARN, "Unpack error");
                    goto fail;
                }

                rc = pki_privkey_build_rsa(key, n, e, d, iqmp, p, q);
#ifdef DEBUG_CRYPTO
                ssh_log_hexdump("n", ssh_string_data(n), ssh_string_len(n));
                ssh_log_hexdump("e", ssh_string_data(e), ssh_string_len(e));
                ssh_log_hexdump("d", ssh_string_data(d), ssh_string_len(d));
                ssh_log_hexdump("iqmp", ssh_string_data(iqmp),
                               ssh_string_len(iqmp));
                ssh_log_hexdump("p", ssh_string_data(p), ssh_string_len(p));
                ssh_log_hexdump("q", ssh_string_data(q), ssh_string_len(q));
#endif
                ssh_string_burn(n);
                SSH_STRING_FREE(n);
                ssh_string_burn(e);
                SSH_STRING_FREE(e);
                ssh_string_burn(d);
                SSH_STRING_FREE(d);
                ssh_string_burn(iqmp);
                SSH_STRING_FREE(iqmp);
                ssh_string_burn(p);
                SSH_STRING_FREE(p);
                ssh_string_burn(q);
                SSH_STRING_FREE(q);
                if (rc == SSH_ERROR) {
                    SSH_LOG(SSH_LOG_WARN, "Failed to build RSA private key");
                    goto fail;
                }
            }
            break;
#ifdef HAVE_ECC
        case SSH_KEYTYPE_ECDSA_P256:
        case SSH_KEYTYPE_ECDSA_P384:
        case SSH_KEYTYPE_ECDSA_P521:
#ifdef WITH_POST_QUANTUM_CRYPTO
        CASE_KEY_ECDSA_HYBRID:
#endif
            {
                ssh_string e = NULL;
                ssh_string exp = NULL;
                ssh_string i = NULL;
                int nid;

                rc = ssh_buffer_unpack(buffer, "SSS", &i, &e, &exp);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_WARN, "Unpack error");
                    goto fail;
                }

                nid = pki_key_ecdsa_nid_from_name(ssh_string_get_char(i));
                SSH_STRING_FREE(i);
                if (nid == -1) {
                    ssh_string_burn(e);
                    SSH_STRING_FREE(e);
                    ssh_string_burn(exp);
                    SSH_STRING_FREE(exp);
                    goto fail;
                }

                rc = pki_privkey_build_ecdsa(key, nid, e, exp);
                ssh_string_burn(e);
                SSH_STRING_FREE(e);
                ssh_string_burn(exp);
                SSH_STRING_FREE(exp);
                if (rc < 0) {
                    SSH_LOG(SSH_LOG_WARN, "Failed to build ECDSA private key");
                    goto fail;
                }
            }
            break;
#endif
        case SSH_KEYTYPE_ED25519:
            {
                ssh_string pubkey = NULL, privkey = NULL;

                rc = ssh_buffer_unpack(buffer, "SS", &pubkey, &privkey);
                if (rc != SSH_OK){
                    SSH_LOG(SSH_LOG_WARN, "Unpack error");
                    goto fail;
                }

                rc = pki_privkey_build_ed25519(key, pubkey, privkey);
                ssh_string_burn(privkey);
                SSH_STRING_FREE(privkey);
                SSH_STRING_FREE(pubkey);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_WARN, "Failed to build ed25519 key");
                    goto fail;
                }
            }
            break;
#ifdef WITH_POST_QUANTUM_CRYPTO
        CASE_KEY_OQS:
            /* Processing happens below, after the switch statement. */
            break;
#endif
        case SSH_KEYTYPE_DSS_CERT01:
        case SSH_KEYTYPE_RSA_CERT01:
        case SSH_KEYTYPE_ECDSA_P256_CERT01:
        case SSH_KEYTYPE_ECDSA_P384_CERT01:
        case SSH_KEYTYPE_ECDSA_P521_CERT01:
        case SSH_KEYTYPE_ED25519_CERT01:
        case SSH_KEYTYPE_SK_ECDSA:
        case SSH_KEYTYPE_SK_ECDSA_CERT01:
        case SSH_KEYTYPE_SK_ED25519:
        case SSH_KEYTYPE_SK_ED25519_CERT01:
        case SSH_KEYTYPE_RSA1:
        case SSH_KEYTYPE_UNKNOWN:
        default:
            SSH_LOG(SSH_LOG_WARN, "Unknown private key type (%d)", type);
            goto fail;
    }

#ifdef WITH_POST_QUANTUM_CRYPTO
    /* Check if we need to handle a PQ or hybrid key. */
    switch (type) {
        CASE_KEY_OQS:
        CASE_KEY_HYBRID:
            {
                ssh_string oqs_pk = NULL, oqs_sk = NULL;
                const char* oqskeytype = pki_get_oqs_alg_name(type);
                if (oqskeytype == NULL) {
                    SSH_LOG(SSH_LOG_WARN, "Could not convert algorithm to OQS type name");
                    goto fail;
                }

                /* Always set type_c again; some types of classical keys will cause this to be changed to the classical type. */
                key->type_c = ssh_key_type_to_char(type);

                key->oqs_sig = OQS_SIG_new(pki_get_oqs_alg_name(type));
                if (key->oqs_sig == NULL) {
                    SSH_LOG(SSH_LOG_TRACE, "Out of memory");
                    goto fail;
                }

                rc = ssh_buffer_unpack(buffer, "SS", &oqs_pk, &oqs_sk);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_TRACE,
                            "Unpack error during OQS key unpack: %d",
                            rc);
                    goto fail;
                }

                if (ssh_string_len(oqs_pk) != key->oqs_sig->length_public_key || ssh_string_len(oqs_sk) != key->oqs_sig->length_secret_key) {
                    SSH_LOG(SSH_LOG_TRACE,
                            "Unpacked key lengths are unexpected; oqs_pk len is %zu, length_public_key is %zu; oqs_sk len is %zu, length_secret_key is %zu",
                            ssh_string_len(oqs_pk), key->oqs_sig->length_public_key, ssh_string_len(oqs_sk), key->oqs_sig->length_secret_key);
                    goto fail;
                }

                key->oqs_pk = malloc(ssh_string_len(oqs_pk));
                if (key->oqs_pk == NULL) {
                    SSH_LOG(SSH_LOG_TRACE, "Out of memory");
                    ssh_string_burn(oqs_pk);
                    ssh_string_burn(oqs_sk);
                    SSH_STRING_FREE(oqs_pk);
                    SSH_STRING_FREE(oqs_sk);
                    goto fail;
                }

                key->oqs_sk = malloc(ssh_string_len(oqs_sk));
                if (key->oqs_sk == NULL) {
                    ssh_string_burn(oqs_pk);
                    ssh_string_burn(oqs_sk);
                    SSH_STRING_FREE(oqs_pk);
                    SSH_STRING_FREE(oqs_sk);
                    SSH_LOG(SSH_LOG_TRACE, "Out of memory");
                    goto fail;
                }

                memcpy(key->oqs_pk, ssh_string_data(oqs_pk), ssh_string_len(oqs_pk));
                memcpy(key->oqs_sk, ssh_string_data(oqs_sk), ssh_string_len(oqs_sk));

                ssh_string_burn(oqs_pk);
                ssh_string_burn(oqs_sk);
                SSH_STRING_FREE(oqs_pk);
                SSH_STRING_FREE(oqs_sk);
                break;
            }
        default:
            /* Nothing to do for purely classical keys. */
            break;
    }
#endif

    *pkey = key;
    return SSH_OK;
fail:
    ssh_key_free(key);

    return SSH_ERROR;
}

static int pki_import_pubkey_buffer(ssh_buffer buffer,
                                    enum ssh_keytypes_e type,
                                    ssh_key *pkey) {
    ssh_key key = NULL;
    int rc;

    key = ssh_key_new();
    if (key == NULL) {
        return SSH_ERROR;
    }

    key->type = type;
    key->type_c = ssh_key_type_to_char(type);
    key->flags = SSH_KEY_FLAG_PUBLIC;

    switch (type) {
        case SSH_KEYTYPE_DSS:
            {
                ssh_string p = NULL;
                ssh_string q = NULL;
                ssh_string g = NULL;
                ssh_string pubkey = NULL;

                rc = ssh_buffer_unpack(buffer, "SSSS", &p, &q, &g, &pubkey);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_WARN, "Unpack error");
                    goto fail;
                }

                rc = pki_pubkey_build_dss(key, p, q, g, pubkey);
#ifdef DEBUG_CRYPTO
                ssh_log_hexdump("p", ssh_string_data(p), ssh_string_len(p));
                ssh_log_hexdump("q", ssh_string_data(q), ssh_string_len(q));
                ssh_log_hexdump("g", ssh_string_data(g), ssh_string_len(g));
#endif
                ssh_string_burn(p);
                SSH_STRING_FREE(p);
                ssh_string_burn(q);
                SSH_STRING_FREE(q);
                ssh_string_burn(g);
                SSH_STRING_FREE(g);
                ssh_string_burn(pubkey);
                SSH_STRING_FREE(pubkey);
                if (rc == SSH_ERROR) {
                    SSH_LOG(SSH_LOG_WARN, "Failed to build DSA public key");
                    goto fail;
                }
            }
            break;
        case SSH_KEYTYPE_RSA:
#ifdef WITH_POST_QUANTUM_CRYPTO
        CASE_KEY_RSA_HYBRID: /* Handle RSA portion of hybrid key here */
#endif
            {
                ssh_string e = NULL;
                ssh_string n = NULL;

                rc = ssh_buffer_unpack(buffer, "SS", &e, &n);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_WARN, "Unpack error");
                    goto fail;
                }

                rc = pki_pubkey_build_rsa(key, e, n);
#ifdef DEBUG_CRYPTO
                ssh_log_hexdump("e", ssh_string_data(e), ssh_string_len(e));
                ssh_log_hexdump("n", ssh_string_data(n), ssh_string_len(n));
#endif
                ssh_string_burn(e);
                SSH_STRING_FREE(e);
                ssh_string_burn(n);
                SSH_STRING_FREE(n);
                if (rc == SSH_ERROR) {
                    SSH_LOG(SSH_LOG_WARN, "Failed to build RSA public key");
                    goto fail;
                }
            }
            break;
#ifdef HAVE_ECC
        case SSH_KEYTYPE_ECDSA: /* deprecated */
        case SSH_KEYTYPE_ECDSA_P256:
        case SSH_KEYTYPE_ECDSA_P384:
        case SSH_KEYTYPE_ECDSA_P521:
        case SSH_KEYTYPE_SK_ECDSA:
#ifdef WITH_POST_QUANTUM_CRYPTO
        CASE_KEY_ECDSA_HYBRID: /* Handle ECC portion of hybrid key here */
#endif
            {
                ssh_string e = NULL;
                ssh_string i = NULL;
                int nid;

                rc = ssh_buffer_unpack(buffer, "SS", &i, &e);
                if (rc != SSH_OK) {
                    SSH_LOG(SSH_LOG_WARN, "Unpack error");
                    goto fail;
                }

                nid = pki_key_ecdsa_nid_from_name(ssh_string_get_char(i));
                SSH_STRING_FREE(i);
                if (nid == -1) {
                    ssh_string_burn(e);
                    SSH_STRING_FREE(e);
                    goto fail;
                }

                rc = pki_pubkey_build_ecdsa(key, nid, e);
                ssh_string_burn(e);
                SSH_STRING_FREE(e);
                if (rc < 0) {
                    SSH_LOG(SSH_LOG_WARN, "Failed to build ECDSA public key");
                    goto fail;
                }

                /* Update key type */
                if (type == SSH_KEYTYPE_ECDSA) {
                    key->type_c = ssh_pki_key_ecdsa_name(key);
                }

                /* Unpack SK specific parameters */
                if (type == SSH_KEYTYPE_SK_ECDSA) {
                    ssh_string application = ssh_buffer_get_ssh_string(buffer);
                    if (application == NULL) {
                        SSH_LOG(SSH_LOG_WARN, "SK Unpack error");
                        goto fail;
                    }
                    key->sk_application = application;
                    key->type_c = ssh_key_type_to_char(key->type);
                }
            }
            break;
#endif
        case SSH_KEYTYPE_ED25519:
        case SSH_KEYTYPE_SK_ED25519:
        {
            ssh_string pubkey = ssh_buffer_get_ssh_string(buffer);

            if (ssh_string_len(pubkey) != ED25519_KEY_LEN) {
                SSH_LOG(SSH_LOG_WARN, "Invalid public key length");
                ssh_string_burn(pubkey);
                SSH_STRING_FREE(pubkey);
                goto fail;
            }

            key->ed25519_pubkey = malloc(ED25519_KEY_LEN);
            if (key->ed25519_pubkey == NULL) {
                ssh_string_burn(pubkey);
                SSH_STRING_FREE(pubkey);
                goto fail;
            }

            memcpy(key->ed25519_pubkey, ssh_string_data(pubkey), ED25519_KEY_LEN);
            ssh_string_burn(pubkey);
            SSH_STRING_FREE(pubkey);

            if (type == SSH_KEYTYPE_SK_ED25519) {
                ssh_string application = ssh_buffer_get_ssh_string(buffer);
                if (application == NULL) {
                    SSH_LOG(SSH_LOG_WARN, "SK Unpack error");
                    goto fail;
                }
                key->sk_application = application;
            }
        }
        break;
#ifdef WITH_POST_QUANTUM_CRYPTO
        CASE_KEY_OQS:
            /* Do nothing here, but don't fail. Processing of PQ and PQ part of hybrid keys is handled below. */
            break;
#endif
        case SSH_KEYTYPE_DSS_CERT01:
        case SSH_KEYTYPE_RSA_CERT01:
        case SSH_KEYTYPE_ECDSA_P256_CERT01:
        case SSH_KEYTYPE_ECDSA_P384_CERT01:
        case SSH_KEYTYPE_ECDSA_P521_CERT01:
        case SSH_KEYTYPE_SK_ECDSA_CERT01:
        case SSH_KEYTYPE_ED25519_CERT01:
        case SSH_KEYTYPE_SK_ED25519_CERT01:
        case SSH_KEYTYPE_RSA1:
        case SSH_KEYTYPE_UNKNOWN:
        default:
            SSH_LOG(SSH_LOG_WARN, "Unknown public key protocol %d", type);
            goto fail;
    }

#ifdef WITH_POST_QUANTUM_CRYPTO
    /* Post-quantum public key handling here. */
    if (IS_OQS_KEY_TYPE(type)) {
        ssh_string pqpubkey;
        const char* oqskeytype = pki_get_oqs_alg_name(type);

        if (oqskeytype == NULL) {
            SSH_LOG(SSH_LOG_WARN, "Could not convert algorithm to OQS type name");
            goto fail;
        }

        key->oqs_sig = OQS_SIG_new(oqskeytype);
        if (key->oqs_sig == NULL) {
            SSH_LOG(SSH_LOG_WARN, "Could not allocate PQ signature key");
            goto fail;
        }

        pqpubkey = ssh_buffer_get_ssh_string(buffer);

        if (ssh_string_len(pqpubkey) != key->oqs_sig->length_public_key) {
            SSH_LOG(SSH_LOG_WARN, "Invalid public key length");
            ssh_string_burn(pqpubkey);
            SSH_STRING_FREE(pqpubkey);
            goto fail;
        }

        key->oqs_pk = malloc(key->oqs_sig->length_public_key);
        if (key->oqs_pk == NULL) {
            ssh_string_burn(pqpubkey);
            SSH_STRING_FREE(pqpubkey);
            goto fail;
        }

        memcpy(key->oqs_pk, ssh_string_data(pqpubkey), key->oqs_sig->length_public_key);
        ssh_string_burn(pqpubkey);
        SSH_STRING_FREE(pqpubkey);

        /* For hybrid keys, this gets set when the classical part key is parsed above. Change it back to the hybrid. */
        key->type_c = ssh_key_type_to_char(type);
    }
#endif

    *pkey = key;
    return SSH_OK;
fail:
    ssh_key_free(key);

    return SSH_ERROR;
}

static int pki_import_cert_buffer(ssh_buffer buffer,
                                  enum ssh_keytypes_e type,
                                  ssh_key *pkey) {
    ssh_buffer cert;
    ssh_string tmp_s;
    const char *type_c;
    ssh_key key = NULL;
    int rc;

    /*
     * The cert blob starts with the key type as an ssh_string, but this
     * string has been read out of the buffer to identify the key type.
     * Simply add it again as first element before copying the rest.
     */
    cert = ssh_buffer_new();
    if (cert == NULL) {
        goto fail;
    }
    type_c = ssh_key_type_to_char(type);
    tmp_s = ssh_string_from_char(type_c);
    if (tmp_s == NULL) {
        goto fail;
    }
    rc = ssh_buffer_add_ssh_string(cert, tmp_s);
    SSH_STRING_FREE(tmp_s);
    if (rc != 0) {
        goto fail;
    }
    rc = ssh_buffer_add_buffer(cert, buffer);
    if (rc != 0) {
        goto fail;
    }

    /*
     * After the key type, comes an ssh_string nonce. Just after this comes the
     * cert public key, which can be parsed out of the buffer.
     */
    tmp_s = ssh_buffer_get_ssh_string(buffer);
    if (tmp_s == NULL) {
        goto fail;
    }
    SSH_STRING_FREE(tmp_s);

    switch (type) {
        case SSH_KEYTYPE_DSS_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_DSS, &key);
            break;
        case SSH_KEYTYPE_RSA_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_RSA, &key);
            break;
        case SSH_KEYTYPE_ECDSA_P256_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_ECDSA_P256, &key);
            break;
        case SSH_KEYTYPE_ECDSA_P384_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_ECDSA_P384, &key);
            break;
        case SSH_KEYTYPE_ECDSA_P521_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_ECDSA_P521, &key);
            break;
        case SSH_KEYTYPE_ED25519_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_ED25519, &key);
            break;
        case SSH_KEYTYPE_SK_ECDSA_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_SK_ECDSA, &key);
            break;
        case SSH_KEYTYPE_SK_ED25519_CERT01:
            rc = pki_import_pubkey_buffer(buffer, SSH_KEYTYPE_SK_ED25519, &key);
            break;
        default:
            key = ssh_key_new();
    }
    if (rc != 0 || key == NULL) {
        goto fail;
    }

    key->type = type;
    key->type_c = type_c;
    key->cert = (void*) cert;

    *pkey = key;
    return SSH_OK;

fail:
    ssh_key_free(key);
    SSH_BUFFER_FREE(cert);
    return SSH_ERROR;
}

/**
 * @brief Import a base64 formated public key from a memory c-string.
 *
 * @param[in]  b64_key  The base64 key to format.
 *
 * @param[in]  type     The type of the key to format.
 *
 * @param[out] pkey     A pointer where the allocated key can be stored. You
 *                      need to free the memory.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_pubkey_base64(const char *b64_key,
                                 enum ssh_keytypes_e type,
                                 ssh_key *pkey) {
    ssh_buffer buffer = NULL;
    ssh_string type_s = NULL;
    int rc;

    if (b64_key == NULL || pkey == NULL) {
        return SSH_ERROR;
    }

    buffer = base64_to_bin(b64_key);
    if (buffer == NULL) {
        return SSH_ERROR;
    }

    type_s = ssh_buffer_get_ssh_string(buffer);
    if (type_s == NULL) {
        SSH_BUFFER_FREE(buffer);
        return SSH_ERROR;
    }
    SSH_STRING_FREE(type_s);

    if (is_cert_type(type)) {
        rc = pki_import_cert_buffer(buffer, type, pkey);
    } else {
        rc = pki_import_pubkey_buffer(buffer, type, pkey);
    }
    SSH_BUFFER_FREE(buffer);

    return rc;
}

/**
 * @internal
 *
 * @brief Import a public key from a ssh string.
 *
 * @param[in]  key_blob The key blob to import as specified in RFC 4253 section
 *                      6.6 "Public Key Algorithms".
 *
 * @param[out] pkey     A pointer where the allocated key can be stored. You
 *                      need to free the memory.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_pubkey_blob(const ssh_string key_blob,
                               ssh_key *pkey) {
    ssh_buffer buffer = NULL;
    ssh_string type_s = NULL;
    enum ssh_keytypes_e type;
    int rc;

    if (key_blob == NULL || pkey == NULL) {
        return SSH_ERROR;
    }

    buffer = ssh_buffer_new();
    if (buffer == NULL) {
        SSH_LOG(SSH_LOG_WARN, "Out of memory!");
        return SSH_ERROR;
    }

    rc = ssh_buffer_add_data(buffer, ssh_string_data(key_blob),
            ssh_string_len(key_blob));
    if (rc < 0) {
        SSH_LOG(SSH_LOG_WARN, "Out of memory!");
        goto fail;
    }

    type_s = ssh_buffer_get_ssh_string(buffer);
    if (type_s == NULL) {
        SSH_LOG(SSH_LOG_WARN, "Out of memory!");
        goto fail;
    }

    type = ssh_key_type_from_name(ssh_string_get_char(type_s));
    if (type == SSH_KEYTYPE_UNKNOWN) {
        SSH_LOG(SSH_LOG_WARN, "Unknown key type found!");
        goto fail;
    }
    SSH_STRING_FREE(type_s);

    if (is_cert_type(type)) {
        rc = pki_import_cert_buffer(buffer, type, pkey);
    } else {
        rc = pki_import_pubkey_buffer(buffer, type, pkey);
    }

    SSH_BUFFER_FREE(buffer);

    return rc;
fail:
    SSH_BUFFER_FREE(buffer);
    SSH_STRING_FREE(type_s);

    return SSH_ERROR;
}

/**
 *@brief Detect if the pathname in cmp is a PKCS #11 URI.
 *
 * @param[in] cmp The path to the public/private key
 *                     or a private/public PKCS #11 URI.
 *
 * @returns true if filename is a URI starting with "pkcs11:"
 *          false otherwise.
 */
bool ssh_pki_is_uri(const char *cmp)
{
    int rc;

    rc = strncmp(cmp, PKCS11_URI, strlen(PKCS11_URI));
    if (rc == 0) {
        return true;
    }

    return false;
}

/**
 *@brief export a Public PKCS #11 URI from a Private PKCS #11 URI
 *       by replacing "type=private" to "type=public".
 *       TODO: Improve the parser
 *
 * @param[in] priv_uri Private PKCS #11 URI.
 *
 * @returns pointer to the public PKCS #11 URI
 */
char *ssh_pki_export_pub_uri_from_priv_uri(const char *priv_uri)
{
    char *pub_uri_temp = NULL;

    pub_uri_temp = ssh_strreplace(priv_uri,
                                  "type=private",
                                  "type=public");

    return pub_uri_temp;
}

/**
 * @brief Import a public key from a file or a PKCS #11 device.
 *
 * @param[in]  filename The filename of the public key or the
 *                      PKCS #11 URI corresponding to the public key.
 *
 * @param[out] pkey     A pointer to store the allocated public key. You need to
 *                      free the memory.
 *
 * @returns SSH_OK on success, SSH_EOF if the file doesn't exist or permission
 *          denied, SSH_ERROR otherwise.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_pubkey_file(const char *filename, ssh_key *pkey)
{
    enum ssh_keytypes_e type;
    struct stat sb;
    char *key_buf, *p;
    size_t buflen, i;
    const char *q;
    FILE *file;
    off_t size;
    int rc, cmp;

    if (pkey == NULL || filename == NULL || *filename == '\0') {
        return SSH_ERROR;
    }

#ifdef WITH_PKCS11_URI
    if (ssh_pki_is_uri(filename)) {
        rc = pki_uri_import(filename, pkey, SSH_KEY_PUBLIC);
        return rc;
    }
#endif

    file = fopen(filename, "rb");
    if (file == NULL) {
        SSH_LOG(SSH_LOG_WARN, "Error opening %s: %s",
                    filename, strerror(errno));
        return SSH_EOF;
    }

    rc = fstat(fileno(file), &sb);
    if (rc < 0) {
        fclose(file);
        SSH_LOG(SSH_LOG_WARN, "Error gettint stat of %s: %s",
                    filename, strerror(errno));
        switch (errno) {
            case ENOENT:
            case EACCES:
                return SSH_EOF;
        }
        return SSH_ERROR;
    }

    if (sb.st_size > MAX_PUBKEY_SIZE) {
        fclose(file);
        return SSH_ERROR;
    }

    key_buf = malloc(sb.st_size + 1);
    if (key_buf == NULL) {
        fclose(file);
        SSH_LOG(SSH_LOG_WARN, "Out of memory!");
        return SSH_ERROR;
    }

    size = fread(key_buf, 1, sb.st_size, file);
    fclose(file);

    if (size != sb.st_size) {
        SAFE_FREE(key_buf);
        SSH_LOG(SSH_LOG_WARN, "Error reading %s: %s",
                    filename, strerror(errno));
        return SSH_ERROR;
    }
    key_buf[size] = '\0';
    buflen = strlen(key_buf);

    /* Test for new OpenSSH key format first */
    cmp = strncmp(key_buf, OPENSSH_HEADER_BEGIN, strlen(OPENSSH_HEADER_BEGIN));
    if (cmp == 0) {
        *pkey = ssh_pki_openssh_pubkey_import(key_buf);
        SAFE_FREE(key_buf);
        if (*pkey == NULL) {
            SSH_LOG(SSH_LOG_WARN, "Failed to import public key from OpenSSH"
                                  " private key file");
            return SSH_ERROR;
        }
        return SSH_OK;
    }

    /* This the old one-line public key format */
    q = p = key_buf;
    for (i = 0; i < buflen; i++) {
        if (isspace((int)p[i])) {
            p[i] = '\0';
            break;
        }
    }

    type = ssh_key_type_from_name(q);
    if (type == SSH_KEYTYPE_UNKNOWN) {
        SAFE_FREE(key_buf);
        return SSH_ERROR;
    }

    q = &p[i + 1];
    for (; i < buflen; i++) {
        if (isspace((int)p[i])) {
            p[i] = '\0';
            break;
        }
    }

    rc = ssh_pki_import_pubkey_base64(q, type, pkey);
    SAFE_FREE(key_buf);

    return rc;
}

/**
 * @brief Import a base64 formated certificate from a memory c-string.
 *
 * @param[in]  b64_cert  The base64 cert to format.
 *
 * @param[in]  type     The type of the cert to format.
 *
 * @param[out] pkey     A pointer where the allocated key can be stored. You
 *                      need to free the memory.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_cert_base64(const char *b64_cert,
                               enum ssh_keytypes_e type,
                               ssh_key *pkey) {
    return ssh_pki_import_pubkey_base64(b64_cert, type, pkey);
}

/**
 * @internal
 *
 * @brief Import a certificate from a ssh string.
 *
 * @param[in]  cert_blob The cert blob to import as specified in RFC 4253 section
 *                      6.6 "Public Key Algorithms".
 *
 * @param[out] pkey     A pointer where the allocated key can be stored. You
 *                      need to free the memory.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_cert_blob(const ssh_string cert_blob,
                             ssh_key *pkey) {
    return ssh_pki_import_pubkey_blob(cert_blob, pkey);
}

/**
 * @brief Import a certificate from the given filename.
 *
 * @param[in]  filename The path to the certificate.
 *
 * @param[out] pkey     A pointer to store the allocated certificate. You need to
 *                      free the memory.
 *
 * @returns SSH_OK on success, SSH_EOF if the file doesn't exist or permission
 *          denied, SSH_ERROR otherwise.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_cert_file(const char *filename, ssh_key *pkey)
{
    return ssh_pki_import_pubkey_file(filename, pkey);
}

/**
 * @brief Generates a keypair.
 *
 * @param[in] type      Type of key to create
 *
 * @param[in] parameter Parameter to the creation of key:
 *                      rsa : length of the key in bits (e.g. 1024, 2048, 4096)
 *                      dsa : length of the key in bits (e.g. 1024, 2048, 3072)
 * @param[out] pkey     A pointer to store the allocated private key. You need
 *                      to free the memory.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @warning             Generating a key pair may take some time.
 */
int ssh_pki_generate(enum ssh_keytypes_e type, int parameter,
        ssh_key *pkey){
    int rc;
    ssh_key key = ssh_key_new();

    if (key == NULL) {
        return SSH_ERROR;
    }

    key->type = type;
    key->type_c = ssh_key_type_to_char(type);
    key->flags = SSH_KEY_FLAG_PRIVATE | SSH_KEY_FLAG_PUBLIC;

    switch(type){
        case SSH_KEYTYPE_RSA:
            rc = pki_key_generate_rsa(key, parameter);
            if(rc == SSH_ERROR)
                goto error;
            break;
        case SSH_KEYTYPE_DSS:
            rc = pki_key_generate_dss(key, parameter);
            if(rc == SSH_ERROR)
                goto error;
            break;
#ifdef HAVE_ECC
        case SSH_KEYTYPE_ECDSA: /* deprecated */
            rc = pki_key_generate_ecdsa(key, parameter);
            if (rc == SSH_ERROR) {
                goto error;
            }

            /* Update key type */
            key->type_c = ssh_pki_key_ecdsa_name(key);
            break;
        case SSH_KEYTYPE_ECDSA_P256:
            rc = pki_key_generate_ecdsa(key, 256);
            if (rc == SSH_ERROR) {
                goto error;
            }
            break;
        case SSH_KEYTYPE_ECDSA_P384:
            rc = pki_key_generate_ecdsa(key, 384);
            if (rc == SSH_ERROR) {
                goto error;
            }
            break;
        case SSH_KEYTYPE_ECDSA_P521:
            rc = pki_key_generate_ecdsa(key, 521);
            if (rc == SSH_ERROR) {
                goto error;
            }
            break;
#endif
        case SSH_KEYTYPE_ED25519:
            rc = pki_key_generate_ed25519(key);
            if (rc == SSH_ERROR) {
                goto error;
            }
            break;
        case SSH_KEYTYPE_DSS_CERT01:
        case SSH_KEYTYPE_RSA_CERT01:
        case SSH_KEYTYPE_ECDSA_P256_CERT01:
        case SSH_KEYTYPE_ECDSA_P384_CERT01:
        case SSH_KEYTYPE_ECDSA_P521_CERT01:
        case SSH_KEYTYPE_ED25519_CERT01:
        case SSH_KEYTYPE_SK_ECDSA:
        case SSH_KEYTYPE_SK_ECDSA_CERT01:
        case SSH_KEYTYPE_SK_ED25519:
        case SSH_KEYTYPE_SK_ED25519_CERT01:
        case SSH_KEYTYPE_RSA1:
        case SSH_KEYTYPE_UNKNOWN:
        default:
            goto error;
    }

    *pkey = key;
    return SSH_OK;
error:
    ssh_key_free(key);
    return SSH_ERROR;
}

/**
 * @brief Create a public key from a private key.
 *
 * @param[in]  privkey  The private key to get the public key from.
 *
 * @param[out] pkey     A pointer to store the newly allocated public key. You
 *                      NEED to free the key.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
int ssh_pki_export_privkey_to_pubkey(const ssh_key privkey,
                                     ssh_key *pkey)
{
    ssh_key pubkey;

    if (privkey == NULL || !ssh_key_is_private(privkey)) {
        return SSH_ERROR;
    }

    pubkey = pki_key_dup(privkey, 1);
    if (pubkey == NULL) {
        return SSH_ERROR;
    }

    *pkey = pubkey;
    return SSH_OK;
}

/**
 * @internal
 *
 * @brief Create a key_blob from a public key.
 *
 * The "key_blob" is encoded as per RFC 4253 section 6.6 "Public Key
 * Algorithms" for any of the supported protocol 2 key types.
 * Encoding of EC keys is described in RFC 5656 section 3.1 "Key
 * Format".
 *
 * @param[in]  key      A public or private key to create the public ssh_string
 *                      from.
 *
 * @param[out] pblob    A pointer to store the newly allocated key blob. You
 *                      NEED to free it.
 *
 * @return              SSH_OK on success, SSH_ERROR otherwise.
 *
 * @see SSH_STRING_FREE()
 */
int ssh_pki_export_pubkey_blob(const ssh_key key,
                               ssh_string *pblob)
{
    ssh_string blob;

    if (key == NULL) {
        return SSH_OK;
    }

    blob = pki_publickey_to_blob(key);
    if (blob == NULL) {
        return SSH_ERROR;
    }

    *pblob = blob;
    return SSH_OK;
}

/**
 * @brief Convert a public key to a base64 encoded key.
 *
 * @param[in] key       The key to hash
 *
 * @param[out] b64_key  A pointer to store the allocated base64 encoded key. You
 *                      need to free the buffer.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see SSH_STRING_FREE_CHAR()
 */
int ssh_pki_export_pubkey_base64(const ssh_key key,
                                 char **b64_key)
{
    ssh_string key_blob;
    unsigned char *b64;

    if (key == NULL || b64_key == NULL) {
        return SSH_ERROR;
    }

    key_blob = pki_publickey_to_blob(key);
    if (key_blob == NULL) {
        return SSH_ERROR;
    }

    b64 = bin_to_base64(ssh_string_data(key_blob), ssh_string_len(key_blob));
    SSH_STRING_FREE(key_blob);
    if (b64 == NULL) {
        return SSH_ERROR;
    }

    *b64_key = (char *)b64;

    return SSH_OK;
}

int ssh_pki_export_pubkey_file(const ssh_key key,
                               const char *filename)
{
    char key_buf[MAX_LINE_SIZE];
    char host[256];
    char *b64_key;
    char *user;
    FILE *fp;
    int rc;

    if (key == NULL || filename == NULL || *filename == '\0') {
        return SSH_ERROR;
    }

    user = ssh_get_local_username();
    if (user == NULL) {
        return SSH_ERROR;
    }

    rc = gethostname(host, sizeof(host));
    if (rc < 0) {
        free(user);
        return SSH_ERROR;
    }

    rc = ssh_pki_export_pubkey_base64(key, &b64_key);
    if (rc < 0) {
        free(user);
        return SSH_ERROR;
    }

    rc = snprintf(key_buf, sizeof(key_buf),
                  "%s %s %s@%s\n",
                  key->type_c,
                  b64_key,
                  user,
                  host);
    free(user);
    free(b64_key);
    if (rc < 0) {
        return SSH_ERROR;
    }

    fp = fopen(filename, "wb+");
    if (fp == NULL) {
        return SSH_ERROR;
    }
    rc = fwrite(key_buf, strlen(key_buf), 1, fp);
    if (rc != 1 || ferror(fp)) {
        fclose(fp);
        unlink(filename);
        return SSH_ERROR;
    }
    fclose(fp);

    return SSH_OK;
}

/**
 * @brief Copy the certificate part of a public key into a private key.
 *
 * @param[in]  certkey  The certificate key.
 *
 * @param[in]  privkey  The target private key to copy the certificate to.
 *
 * @returns SSH_OK on success, SSH_ERROR otherwise.
 **/
int ssh_pki_copy_cert_to_privkey(const ssh_key certkey, ssh_key privkey) {
  ssh_buffer cert_buffer;
  int rc;

  if (certkey == NULL || privkey == NULL) {
      return SSH_ERROR;
  }

  if (privkey->cert != NULL) {
      return SSH_ERROR;
  }

  if (certkey->cert == NULL) {
      return SSH_ERROR;
  }

  cert_buffer = ssh_buffer_new();
  if (cert_buffer == NULL) {
      return SSH_ERROR;
  }

  rc = ssh_buffer_add_buffer(cert_buffer, certkey->cert);
  if (rc != 0) {
      SSH_BUFFER_FREE(cert_buffer);
      return SSH_ERROR;
  }

  privkey->cert = cert_buffer;
  privkey->cert_type = certkey->type;
  return SSH_OK;
}

int ssh_pki_export_signature_blob(const ssh_signature sig,
                                  ssh_string *sig_blob)
{
    ssh_buffer buf = NULL;
    ssh_string str;
#ifdef WITH_POST_QUANTUM_CRYPTO
    ssh_string sig_classical = NULL;
    unsigned char *sig_data = NULL;
#endif
    int rc;

    if (sig == NULL || sig_blob == NULL) {
        return SSH_ERROR;
    }

    buf = ssh_buffer_new();
    if (buf == NULL) {
        return SSH_ERROR;
    }

#ifdef WITH_POST_QUANTUM_CRYPTO
    if (!IS_OQS_KEY_TYPE(sig->type) || IS_HYBRID(sig->type)) {
#endif

        str = ssh_string_from_char(sig->type_c);
        if (str == NULL) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        rc = ssh_buffer_add_ssh_string(buf, str);
        SSH_STRING_FREE(str);
        if (rc < 0) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        str = pki_signature_to_blob(sig);
        if (str == NULL) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        rc = ssh_buffer_add_ssh_string(buf, str);
        SSH_STRING_FREE(str);
        if (rc < 0) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        str = ssh_string_new(ssh_buffer_get_len(buf));
        if (str == NULL) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        rc = ssh_string_fill(str, ssh_buffer_get(buf), ssh_buffer_get_len(buf));
        SSH_BUFFER_FREE(buf);
        if (rc < 0) {
             SSH_STRING_FREE(str);
             return SSH_ERROR;
        }

#ifdef WITH_POST_QUANTUM_CRYPTO
        /* If WITH_POST_QUANTUM_CRYPTO is not defined, str will continue to contain the classical signature as before. */
        sig_classical = str;
        str = NULL;
    }

    if (IS_OQS_KEY_TYPE(sig->type)) {
        if (sig->pq_sig == NULL) {
            ssh_string_burn(sig_classical);
            SSH_STRING_FREE(sig_classical);
            return SSH_ERROR;
        }
    }

    switch (sig->type) {
    CASE_KEY_HYBRID:
        /* Hybrid signature is "sig_classical_len|sig_classical|sig_pq_len|sig_pq" where each _len is an unsigned 4-byte big-endian int. */
        if (sig_classical == NULL) {
            /* Sanity check. This shouldn't happen, though, as failures in the classical signature should cause an early exit. */
            return SSH_ERROR;
        }

        str = ssh_string_new(sizeof(uint32_t) + ssh_string_len(sig_classical) + sizeof(uint32_t) + ssh_string_len(sig->pq_sig));
        if (str == NULL) {
            ssh_string_burn(sig_classical);
            SSH_STRING_FREE(sig_classical);
            return SSH_ERROR;
        }
        sig_data = ssh_string_data(str);

        /* Encode length of classical signature. */
        *(uint32_t*)&sig_data[0] = htonl(ssh_string_len(sig_classical));
        /* Copy classical signature. */
        memcpy(&sig_data[sizeof(uint32_t)],
                ssh_string_data(sig_classical),
                ssh_string_len(sig_classical));

        /* Encode length of PQ signature. */
        *(uint32_t*)&sig_data[sizeof(uint32_t) + ssh_string_len(sig_classical)] = htonl(ssh_string_len(sig->pq_sig));
        /* Copy PQ signature. */
        memcpy(&sig_data[sizeof(uint32_t) + ssh_string_len(sig_classical) + sizeof(uint32_t)], 
                ssh_string_data(sig->pq_sig),
                ssh_string_len(sig->pq_sig));

        ssh_string_burn(sig_classical);
        SSH_STRING_FREE(sig_classical);
        sig_classical = NULL;

        break;

    CASE_KEY_OQS:
        str = ssh_string_copy(sig->pq_sig);
        if (str == NULL) {
            /* sig_classical should be unused and NULL, so no need to burn and free it. */
            return SSH_ERROR;
        }
        break;

    default:
        str = sig_classical;
        break;
    }
#endif

    *sig_blob = str;

    return SSH_OK;
}

int ssh_pki_import_signature_blob(const ssh_string sig_blob,
                                  const ssh_key pubkey,
                                  ssh_signature *psig)
{
    ssh_signature sig = NULL;
    enum ssh_keytypes_e type;
    enum ssh_digest_e hash_type;
    ssh_string algorithm = NULL, blob = NULL;
    ssh_buffer buf;
    const char *alg = NULL;
    uint8_t flags = 0;
    uint32_t counter = 0;
    int rc;
    const unsigned char *sig_classical = NULL;
    uint32_t sig_classical_len = 0;
#ifdef WITH_POST_QUANTUM_CRYPTO
    const unsigned char *sig_pq = NULL;
    uint32_t sig_pq_len = 0;
#endif

    if (sig_blob == NULL || psig == NULL) {
        return SSH_ERROR;
    }

#ifdef WITH_POST_QUANTUM_CRYPTO
    if (IS_HYBRID(pubkey->type)) {
        const unsigned char *sig_data = NULL;
        size_t sig_len = ssh_string_len(sig_blob);
        /* Hybrid signature is "sig_classical_len|sig_classical|sig_pq_len|sig_pq" where each _len is an unsigned 4-byte big-endian int */
        if (sig_len < sizeof(sig_classical_len))
        {
            SSH_LOG(SSH_LOG_TRACE,
                    "Not enough space in the signature buffer for sig_classical_len; sig_len is %zu",
                    sig_len);
            return SSH_ERROR;
        }
        sig_data = ssh_string_data(sig_blob);
        sig_classical_len = ntohl(*(const uint32_t*)&sig_data[0]);
        if (sig_len < sig_classical_len + sizeof(sig_classical_len) + sizeof(sig_pq_len))
        {
            SSH_LOG(SSH_LOG_TRACE,
                    "Not enough space in the signature buffer for sig_classical and sig_pq_len; sig_len is %zu, sig_classical_len is %u",
                    sig_len, sig_classical_len);
            return SSH_ERROR;
        }

        sig_classical = &sig_data[sizeof(sig_classical_len)];

        sig_pq_len = ntohl(*(const uint32_t*)&sig_data[sizeof(sig_classical_len) + sig_classical_len]);
        if (sig_len < sizeof(sig_classical_len) + sig_classical_len + sizeof(sig_pq_len) + sig_pq_len)
        {
            SSH_LOG(SSH_LOG_TRACE,
                    "Not enough space in the signature buffer for sig_pq; sig_len is %zu, sig_classical_len is %u, sig_pq_len is %u",
                    sig_len, sig_classical_len, sig_pq_len);
            return SSH_ERROR;
        }

        sig_pq = &sig_data[sizeof(sig_classical_len) + sig_classical_len + sizeof(sig_pq_len)];

        /* Sanity check. Make sure the entire signature blob's length is equal to the lengths of the four components. */
        if (sizeof(sig_classical_len) + sig_classical_len + sizeof(sig_pq_len) + sig_pq_len != sig_len) {
            SSH_LOG(SSH_LOG_TRACE,
                    "Didn't use all of the sig buffer: sig_len is %zu, sig_classical_len is %u, sig_pq_len is %u, total should be %lu",
                    sig_len, sig_classical_len, sig_pq_len, sizeof(sig_classical_len) + sig_classical_len + sizeof(sig_pq_len) + sig_pq_len);
            return SSH_ERROR;
        }
    } else if (IS_OQS_KEY_TYPE(pubkey->type)) {
        /* PQ only, since IS_HYBRID was false. sig_classical stays null. */
        sig_pq = ssh_string_data(sig_blob);
        sig_pq_len = ssh_string_len(sig_blob);
    } else {
        /* Classical only. sig_pq stays null. */
        sig_classical = ssh_string_data(sig_blob);
        sig_classical_len = ssh_string_len(sig_blob);
    }
#else /* WITH_POST_QUANTUM_CRYPTO */
    sig_classical = ssh_string_data(sig_blob);
    sig_classical_len = ssh_string_len(sig_blob);

    if (sig_classical == NULL) {
        return SSH_ERROR;
    }
#endif /* WITH_POST_QUANTUM_CRYPTO */

    if (sig_classical != NULL) {
        buf = ssh_buffer_new();
        if (buf == NULL) {
            return SSH_ERROR;
        }
        rc = ssh_buffer_add_data(buf,
                                 sig_classical,
                                 sig_classical_len);
        if (rc < 0) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        algorithm = ssh_buffer_get_ssh_string(buf);
        if (algorithm == NULL) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        alg = ssh_string_get_char(algorithm);
        type = ssh_key_type_from_signature_name(alg);
        hash_type = ssh_key_hash_from_name(alg);
        SSH_STRING_FREE(algorithm);

        blob = ssh_buffer_get_ssh_string(buf);
        if (blob == NULL) {
            SSH_BUFFER_FREE(buf);
            return SSH_ERROR;
        }

        if (type == SSH_KEYTYPE_SK_ECDSA ||
            type == SSH_KEYTYPE_SK_ED25519) {
            rc = ssh_buffer_unpack(buf, "bd", &flags, &counter);
            if (rc < 0) {
                SSH_BUFFER_FREE(buf);
                SSH_STRING_FREE(blob);
                return SSH_ERROR;
            }
        }
        SSH_BUFFER_FREE(buf);

        sig = pki_signature_from_blob(pubkey, blob, type, hash_type);
        SSH_STRING_FREE(blob);
        if (sig == NULL) {
            return SSH_ERROR;
        }

        /* Set SK specific values */
        sig->sk_flags = flags;
        sig->sk_counter = counter;
    }

#ifdef WITH_POST_QUANTUM_CRYPTO
    /* If PQ signature is present, add to signature structure. */
    if (sig_pq != NULL && sig_pq_len > 0) {
        /* If not a hybrid signature, we need to initialize the signature struct. */
        if (sig == NULL) {
            sig = ssh_signature_new();
            if (sig == NULL) {
                return SSH_ERROR;
            }
        }

        blob = ssh_string_new(sig_pq_len);
        if (blob == NULL) {
            ssh_signature_free(sig);
            return SSH_ERROR;
        }

        rc = ssh_string_fill(blob, sig_pq, sig_pq_len);
        if (rc < 0) {
            ssh_signature_free(sig);
            SSH_STRING_FREE(blob);
            return SSH_ERROR;
        }

        rc = pki_parse_oqs_signature_from_blob(sig, pubkey, blob, type, hash_type);
        if (rc < 0) {
            ssh_signature_free(sig);
            SSH_STRING_FREE(blob);
            return SSH_ERROR;
        }

        SSH_STRING_FREE(blob);
    }
#endif

    *psig = sig;
    return SSH_OK;
}

/**
 * @internal
 *
 * @brief Check if the provided key can be used with the provided hash type for
 * data signing or signature verification.
 *
 * @param[in]   key         The key to be checked.
 * @param[in]   hash_type   The digest algorithm to be checked.
 *
 * @return  SSH_OK if compatible; SSH_ERROR otherwise
 */
int pki_key_check_hash_compatible(ssh_key key,
                                  enum ssh_digest_e hash_type)
{
    if (key == NULL) {
        SSH_LOG(SSH_LOG_TRACE, "Null pointer provided as key to "
                               "pki_key_check_hash_compatible()");
        return SSH_ERROR;
    }

    switch(key->type) {
    case SSH_KEYTYPE_DSS_CERT01:
    case SSH_KEYTYPE_DSS:
        if (hash_type == SSH_DIGEST_SHA1) {
            if (ssh_fips_mode()) {
                SSH_LOG(SSH_LOG_WARN, "SHA1 is not allowed in FIPS mode");
                return SSH_ERROR;
            } else {
                return SSH_OK;
            }
        }
        break;
    case SSH_KEYTYPE_RSA_CERT01:
    case SSH_KEYTYPE_RSA:
#ifdef WITH_POST_QUANTUM_CRYPTO
    CASE_KEY_RSA_HYBRID:
#endif
        if (hash_type == SSH_DIGEST_SHA1) {
            if (ssh_fips_mode()) {
                SSH_LOG(SSH_LOG_WARN, "SHA1 is not allowed in FIPS mode");
                return SSH_ERROR;
            } else {
                return SSH_OK;
            }
        }

        if (hash_type == SSH_DIGEST_SHA256 ||
            hash_type == SSH_DIGEST_SHA512)
        {
            return SSH_OK;
        }
        break;
    case SSH_KEYTYPE_ECDSA_P256_CERT01:
    case SSH_KEYTYPE_ECDSA_P256:
    case SSH_KEYTYPE_SK_ECDSA_CERT01:
    case SSH_KEYTYPE_SK_ECDSA:
#ifdef WITH_POST_QUANTUM_CRYPTO
///// OQS_TEMPLATE_FRAGMENT_HYBRID_SHA256_OK_START
    case SSH_KEYTYPE_P256_DILITHIUM_2:
    case SSH_KEYTYPE_P256_FALCON_512:
    case SSH_KEYTYPE_P256_PICNIC_L1FULL:
    case SSH_KEYTYPE_P256_PICNIC3_L1:
    case SSH_KEYTYPE_P256_SPHINCS_HARAKA_128F_ROBUST:
    case SSH_KEYTYPE_P256_SPHINCS_SHA256_128F_ROBUST:
    case SSH_KEYTYPE_P256_SPHINCS_SHAKE256_128F_ROBUST:
///// OQS_TEMPLATE_FRAGMENT_HYBRID_SHA256_OK_END
#endif
        if (hash_type == SSH_DIGEST_SHA256) {
            return SSH_OK;
        }
        break;
    case SSH_KEYTYPE_ECDSA_P384_CERT01:
    case SSH_KEYTYPE_ECDSA_P384:
#ifdef WITH_POST_QUANTUM_CRYPTO
///// OQS_TEMPLATE_FRAGMENT_HYBRID_SHA384_OK_START
///// OQS_TEMPLATE_FRAGMENT_HYBRID_SHA384_OK_END
#endif
        if (hash_type == SSH_DIGEST_SHA384) {
            return SSH_OK;
        }
        break;
    case SSH_KEYTYPE_ECDSA_P521_CERT01:
    case SSH_KEYTYPE_ECDSA_P521:
#ifdef WITH_POST_QUANTUM_CRYPTO
///// OQS_TEMPLATE_FRAGMENT_HYBRID_SHA512_OK_START
///// OQS_TEMPLATE_FRAGMENT_HYBRID_SHA512_OK_END
#endif
        if (hash_type == SSH_DIGEST_SHA512) {
            return SSH_OK;
        }
        break;
    case SSH_KEYTYPE_ED25519_CERT01:
    case SSH_KEYTYPE_ED25519:
    case SSH_KEYTYPE_SK_ED25519_CERT01:
    case SSH_KEYTYPE_SK_ED25519:
#ifdef WITH_POST_QUANTUM_CRYPTO
    CASE_KEY_OQS:
#endif
        if (hash_type == SSH_DIGEST_AUTO) {
            return SSH_OK;
        }
        break;
    case SSH_KEYTYPE_RSA1:
    case SSH_KEYTYPE_ECDSA:
    case SSH_KEYTYPE_UNKNOWN:
        SSH_LOG(SSH_LOG_WARN, "Unknown key type %d", key->type);
        return SSH_ERROR;
    }

    SSH_LOG(SSH_LOG_WARN, "Key type %d incompatible with hash type  %d",
            key->type, hash_type);

    return SSH_ERROR;
}

int ssh_pki_signature_verify(ssh_session session,
                             ssh_signature sig,
                             const ssh_key key,
                             const unsigned char *input,
                             size_t input_len)
{
    int rc;
    enum ssh_keytypes_e key_type;

    if (session == NULL || sig == NULL || key == NULL || input == NULL) {
        SSH_LOG(SSH_LOG_TRACE, "Bad parameter provided to "
                               "ssh_pki_signature_verify()");
        return SSH_ERROR;
    }
    key_type = ssh_key_type_plain(key->type);

#ifdef WITH_POST_QUANTUM_CRYPTO
    if (!IS_OQS_KEY_TYPE(key_type) || IS_HYBRID(key_type)) {
        /* Only do this if we have a classical signature present, because the key is either pure-classical or hybrid. */
#endif
        SSH_LOG(SSH_LOG_FUNCTIONS,
                "Going to verify a %s type signature",
                sig->type_c);

        if ((key_type != sig->type)
#ifdef WITH_POST_QUANTUM_CRYPTO
            /* RSA hybrids maintain the original classical algorithm string identifier. Allow them to proceed. */
            && !(sig->type == SSH_KEYTYPE_RSA && IS_RSA_HYBRID(key_type))
#endif
            ) {
            SSH_LOG(SSH_LOG_WARN,
                    "Can not verify %s signature with %s key",
                    sig->type_c, key->type_c);
            return SSH_ERROR;
        }

        /* Check if public key and hash type are compatible */
        rc = pki_key_check_hash_compatible(key, sig->hash_type);
        if (rc != SSH_OK) {
            return SSH_ERROR;
        }

        if (key->type == SSH_KEYTYPE_SK_ECDSA ||
            key->type == SSH_KEYTYPE_SK_ECDSA_CERT01 ||
            key->type == SSH_KEYTYPE_SK_ED25519 ||
            key->type == SSH_KEYTYPE_SK_ED25519_CERT01) {

            ssh_buffer sk_buffer = NULL;
            SHA256CTX ctx = NULL;
            unsigned char application_hash[SHA256_DIGEST_LEN] = {0};
            unsigned char input_hash[SHA256_DIGEST_LEN] = {0};

            ctx = sha256_init();
            if (ctx == NULL) {
                SSH_LOG(SSH_LOG_WARN,
                        "Can not create SHA256CTX for application hash");
                return SSH_ERROR;
            }
            sha256_update(ctx, ssh_string_data(key->sk_application),
                    ssh_string_len(key->sk_application));
            sha256_final(application_hash, ctx);

            ctx = sha256_init();
            if (ctx == NULL) {
                SSH_LOG(SSH_LOG_WARN,
                        "Can not create SHA256CTX for input hash");
                return SSH_ERROR;
            }
            sha256_update(ctx, input, input_len);
            sha256_final(input_hash, ctx);

            sk_buffer = ssh_buffer_new();
            if (sk_buffer == NULL) {
                return SSH_ERROR;
            }

            rc = ssh_buffer_pack(sk_buffer, "PbdP",
                                    SHA256_DIGEST_LEN, application_hash,
                                    sig->sk_flags, sig->sk_counter,
                                    SHA256_DIGEST_LEN, input_hash);
            if (rc != SSH_OK) {
                SSH_BUFFER_FREE(sk_buffer);
                explicit_bzero(input_hash, SHA256_DIGEST_LEN);
                explicit_bzero(application_hash, SHA256_DIGEST_LEN);
                return SSH_ERROR;
            }

            rc = pki_verify_data_signature(sig, key, ssh_buffer_get(sk_buffer),
                                            ssh_buffer_get_len(sk_buffer));

            SSH_BUFFER_FREE(sk_buffer);
            explicit_bzero(input_hash, SHA256_DIGEST_LEN);
            explicit_bzero(application_hash, SHA256_DIGEST_LEN);

            return rc;
        }
#ifdef WITH_POST_QUANTUM_CRYPTO
    }
#endif

    return pki_verify_data_signature(sig, key, input, input_len);
}

ssh_signature pki_do_sign(const ssh_key privkey,
                          const unsigned char *input,
                          size_t input_len,
                          enum ssh_digest_e hash_type)
{
    int rc;

    if (privkey == NULL || input == NULL) {
        SSH_LOG(SSH_LOG_TRACE, "Bad parameter provided to "
                               "pki_do_sign()");
        return NULL;
    }

    /* Check if public key and hash type are compatible */
    rc = pki_key_check_hash_compatible(privkey, hash_type);
    if (rc != SSH_OK) {
        return NULL;
    }

    return pki_sign_data(privkey, hash_type, input, input_len);
}

/*
 * This function signs the session id as a string then
 * the content of sigbuf */
ssh_string ssh_pki_do_sign(ssh_session session,
                           ssh_buffer sigbuf,
                           const ssh_key privkey,
                           enum ssh_digest_e hash_type)
{
    struct ssh_crypto_struct *crypto = NULL;

    ssh_signature sig = NULL;
    ssh_string sig_blob = NULL;

    ssh_string session_id = NULL;
    ssh_buffer sign_input = NULL;

    int rc;

    if (session == NULL || sigbuf == NULL || privkey == NULL ||
        !ssh_key_is_private(privkey))
    {
        SSH_LOG(SSH_LOG_TRACE, "Bad parameter provided to "
                               "ssh_pki_do_sign()");
        return NULL;
    }

    crypto = ssh_packet_get_current_crypto(session, SSH_DIRECTION_BOTH);
    if (crypto == NULL) {
        return NULL;
    }

    /* Get the session ID */
    session_id = ssh_string_new(crypto->session_id_len);
    if (session_id == NULL) {
        return NULL;
    }
    rc = ssh_string_fill(session_id, crypto->session_id, crypto->session_id_len);
    if (rc < 0) {
        goto end;
    }

    /* Fill the input */
    sign_input = ssh_buffer_new();
    if (sign_input == NULL) {
        goto end;
    }
    ssh_buffer_set_secure(sign_input);

    rc = ssh_buffer_pack(sign_input,
                         "SP",
                         session_id,
                         ssh_buffer_get_len(sigbuf), ssh_buffer_get(sigbuf));
    if (rc != SSH_OK) {
        goto end;
    }

    /* Generate the signature */
    sig = pki_do_sign(privkey,
            ssh_buffer_get(sign_input),
            ssh_buffer_get_len(sign_input),
            hash_type);
    if (sig == NULL) {
        goto end;
    }

    /* Convert the signature to blob */
    rc = ssh_pki_export_signature_blob(sig, &sig_blob);
    if (rc < 0) {
        sig_blob = NULL;
    }

end:
    ssh_signature_free(sig);
    SSH_BUFFER_FREE(sign_input);
    SSH_STRING_FREE(session_id);

    return sig_blob;
}

#ifndef _WIN32
ssh_string ssh_pki_do_sign_agent(ssh_session session,
                                 struct ssh_buffer_struct *buf,
                                 const ssh_key pubkey)
{
    struct ssh_crypto_struct *crypto = NULL;
    ssh_string session_id;
    ssh_string sig_blob;
    ssh_buffer sig_buf;
    int rc;

    crypto = ssh_packet_get_current_crypto(session, SSH_DIRECTION_BOTH);
    if (crypto == NULL) {
        return NULL;
    }

    /* prepend session identifier */
    session_id = ssh_string_new(crypto->session_id_len);
    if (session_id == NULL) {
        return NULL;
    }
    rc = ssh_string_fill(session_id, crypto->session_id, crypto->session_id_len);
    if (rc < 0) {
        SSH_STRING_FREE(session_id);
        return NULL;
    }

    sig_buf = ssh_buffer_new();
    if (sig_buf == NULL) {
        SSH_STRING_FREE(session_id);
        return NULL;
    }

    rc = ssh_buffer_add_ssh_string(sig_buf, session_id);
    if (rc < 0) {
        SSH_STRING_FREE(session_id);
        SSH_BUFFER_FREE(sig_buf);
        return NULL;
    }
    SSH_STRING_FREE(session_id);

    /* append out buffer */
    if (ssh_buffer_add_buffer(sig_buf, buf) < 0) {
        SSH_BUFFER_FREE(sig_buf);
        return NULL;
    }

    /* create signature */
    sig_blob = ssh_agent_sign_data(session, pubkey, sig_buf);

    SSH_BUFFER_FREE(sig_buf);

    return sig_blob;
}
#endif /* _WIN32 */

#ifdef WITH_SERVER
ssh_string ssh_srv_pki_do_sign_sessionid(ssh_session session,
                                         const ssh_key privkey,
                                         const enum ssh_digest_e digest)
{
    struct ssh_crypto_struct *crypto = NULL;

    ssh_signature sig = NULL;
    ssh_string sig_blob = NULL;

    ssh_buffer sign_input = NULL;

    int rc;

    if (session == NULL || privkey == NULL || !ssh_key_is_private(privkey)) {
        return NULL;
    }

    crypto = session->next_crypto ? session->next_crypto :
                                    session->current_crypto;

    if (crypto->secret_hash == NULL){
        ssh_set_error(session,SSH_FATAL,"Missing secret_hash");
        return NULL;
    }

    /* Fill the input */
    sign_input = ssh_buffer_new();
    if (sign_input == NULL) {
        goto end;
    }
    ssh_buffer_set_secure(sign_input);

    rc = ssh_buffer_pack(sign_input,
                         "P",
                         crypto->digest_len,
                         crypto->secret_hash);
    if (rc != SSH_OK) {
        goto end;
    }

    /* Generate the signature */
    sig = pki_do_sign(privkey,
            ssh_buffer_get(sign_input),
            ssh_buffer_get_len(sign_input),
            digest);
    if (sig == NULL) {
        goto end;
    }

    /* Convert the signature to blob */
    rc = ssh_pki_export_signature_blob(sig, &sig_blob);
    if (rc < 0) {
        sig_blob = NULL;
    }

end:
    ssh_signature_free(sig);
    SSH_BUFFER_FREE(sign_input);

    return sig_blob;
}
#endif /* WITH_SERVER */

/**
 * @}
 */
