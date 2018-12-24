#include <assert.h>
#include <mutex>
#include <sodium.h>

#include <QMetaType>
#include <QUuid>

#include "ds/crypto.h"
#include "ds/dscert.h"
#include "ds/cvar.h"

namespace ds {
namespace crypto {

Crypto::Crypto()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (sodium_init()) {
            throw std::runtime_error("sodium_init() failed");
        }

        qRegisterMetaType<ds::crypto::DsCert::ptr_t>("ds::crypto::DsCert::ptr_t");
        qRegisterMetaType<ds::crypto::DsCert::ptr_t>("DsCert::ptr_t");
    });
}

Crypto::~Crypto()
{
}

QByteArray Crypto::getHmacSha256(const QByteArray& key,
                                 std::initializer_list<const QByteArray*> data)
{
    QByteArray hash;
    hash.fill(0, crypto_auth_hmacsha256_BYTES);
    crypto_auth_hmacsha256_state state = {};
    crypto_auth_hmacsha256_init(&state,
                                reinterpret_cast<const unsigned char *>(key.data()),
                                static_cast<size_t>(key.size()));
    for(const auto& s : data) {
        crypto_auth_hmacsha256_update(&state,
                                      reinterpret_cast<const unsigned char *>(s->data()),
                                      static_cast<size_t>(s->size()));
    }

    crypto_auth_hmacsha256_final(&state,
                                 reinterpret_cast<unsigned char *>(hash.data()));
    return hash;
}

QByteArray Crypto::getSha256(const QByteArray &data)
{
    QByteArray hash;
    hash.fill(0, crypto_hash_sha256_BYTES);
    crypto_hash_sha256(reinterpret_cast<unsigned char *>(hash.data()),
                       reinterpret_cast<const unsigned char *>(data.data()),
                       static_cast<size_t>(data.size()));
    return hash;
}

QByteArray Crypto::generateId()
{
    auto uuid = QUuid::createUuid().toByteArray();
    return crypto::Crypto::getSha256(uuid);
}

}} // namespaces
