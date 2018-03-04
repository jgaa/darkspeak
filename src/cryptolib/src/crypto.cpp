#include <assert.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/conf.h>
 #include <openssl/rand.h>

#include "ds/crypto.h"

namespace ds {
namespace crypto {

Crypto::Crypto()
{
    // Initialize openssl crypto lib
    static bool initialized = false;
    if (!initialized) {
        /* Load the human readable error strings for libcrypto */
          ERR_load_crypto_strings();

          /* Load all digest and cipher algorithms */
          OpenSSL_add_all_algorithms();
          //OPENSSL_config (NULL); depricated in 1.1
    }

    // TODO: Add mt callbacks if we use an openssl libraryn older than 1.1.0
    // https://www.openssl.org/blog/blog/2017/02/21/threads/
    assert(OPENSSL_VERSION_NUMBER >= 0x10001000L);
    assert(RAND_status() == 1);
}

Crypto::~Crypto()
{
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    ERR_free_strings();
}

}} // namespaces
