#include <assert.h>

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <QMetaType>

#include "ds/crypto.h"
#include "ds/dscert.h"
#include "ds/cvar.h"

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

          qRegisterMetaType<ds::crypto::DsCert::ptr_t>("ds::crypto::DsCert::ptr_t");
          qRegisterMetaType<ds::crypto::DsCert::ptr_t>("DsCert::ptr_t");
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

QByteArray Crypto::getHmacSha256(const QByteArray& key,
                                 std::initializer_list<const QByteArray*> data)
{
    QByteArray rval;
    rval.resize(EVP_MAX_MD_SIZE);

    auto ctx = make_cvar(HMAC_CTX_new(), HMAC_CTX_free, "HMAC_CTX_new()");

    auto len = static_cast<unsigned int>(rval.size());
    HMAC_Init_ex(ctx, key.data(), key.size(), EVP_sha256(), NULL);

    for(const auto& d : data) {
        assert(d);
        HMAC_Update(ctx,
                    reinterpret_cast<const unsigned char *>(d->data()),
                    static_cast<size_t>(d->size()));
    }

    HMAC_Final(ctx, reinterpret_cast<unsigned char *>(rval.data()), &len);

    rval.resize(static_cast<int>(len));

    return rval;
}

QByteArray Crypto::getSha256(const QByteArray &data)
{
    QByteArray hash(SHA256_DIGEST_LENGTH, '\0');
    SHA256_CTX sha256 = {};
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data.data(), static_cast<size_t>(data.size()));
    SHA256_Final(reinterpret_cast<unsigned char *>(hash.data()), &sha256);
    return hash;
}

}} // namespaces
