#ifndef CRYPTO_H
#define CRYPTO_H

#include <sodium.h>
#include <QByteArray>


namespace ds {
namespace crypto {


// Initializes sodium library. We need one instance in  main().
class Crypto
{
public:
    Crypto();
    ~Crypto() = default;

    static QByteArray getHmacSha256(const QByteArray& key, std::initializer_list<const QByteArray*> data);
    static QByteArray getSha256(const QByteArray& data);
    static QByteArray generateId();
    static QByteArray getRandomBytes(size_t bytes);
};

template <typename Tout, typename T>
void createHash(Tout& hash, std::initializer_list<T> data) {

    hash.resize(crypto_generichash_BYTES);
    crypto_generichash_state state = {};
    crypto_generichash_init(&state, nullptr, 0, static_cast<size_t>(hash.size()));

    for(const auto& d : data) {
        (void) crypto_generichash_update(
                    &state,
                    reinterpret_cast<const unsigned char *>(d.data()),
                    static_cast<size_t>(d.size()));

    }

    crypto_generichash_final(
                &state,
                reinterpret_cast<unsigned char *>(hash.data()),
                static_cast<size_t>(hash.size()));
}

}} // namespaces

#endif // CRYPTO_H
