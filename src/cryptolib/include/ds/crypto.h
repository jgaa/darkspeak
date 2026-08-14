#ifndef CRYPTO_H
#define CRYPTO_H

#include <sodium.h>
#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <span>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>


namespace ds {
namespace crypto {

namespace detail {

template <typename T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T>
concept Byte = std::same_as<std::remove_cv_t<T>, char>
        || std::same_as<std::remove_cv_t<T>, signed char>
        || std::same_as<std::remove_cv_t<T>, unsigned char>
        || std::same_as<std::remove_cv_t<T>, std::byte>;

template <typename T>
struct is_byte_span : std::false_type {};

template <typename T, std::size_t Extent>
struct is_byte_span<std::span<T, Extent>>
    : std::bool_constant<Byte<T>> {};

template <typename T>
concept ByteSpan = is_byte_span<remove_cvref_t<T>>::value;

template <typename T>
concept Sha256Text = std::same_as<remove_cvref_t<T>, QString>
        || std::same_as<remove_cvref_t<T>, QStringView>
        || std::same_as<remove_cvref_t<T>, std::string>
        || std::same_as<remove_cvref_t<T>, std::string_view>;

template <typename T>
concept Sha256Bytes = std::same_as<remove_cvref_t<T>, QByteArray>
        || std::same_as<remove_cvref_t<T>, QByteArrayView>;

template <typename T>
concept Sha256Part = Sha256Text<T> || Sha256Bytes<T> || ByteSpan<T>;

inline void sha256UpdateBytes(crypto_hash_sha256_state& state,
                              const void *data, const std::size_t size)
{
    if (size != 0) {
        crypto_hash_sha256_update(&state,
                                  static_cast<const unsigned char *>(data),
                                  static_cast<unsigned long long>(size));
    }
}

inline void sha256UpdateBytes(crypto_hash_sha256_state& state,
                              const QByteArrayView bytes)
{
    sha256UpdateBytes(state, bytes.data(), static_cast<std::size_t>(bytes.size()));
}

template <Sha256Part Part>
void sha256Update(crypto_hash_sha256_state& state, const Part& part)
{
    using Value = remove_cvref_t<Part>;
    if constexpr (Sha256Bytes<Value>) {
        sha256UpdateBytes(state, QByteArrayView{part});
    } else if constexpr (std::same_as<Value, QString>
                         || std::same_as<Value, QStringView>) {
        const auto utf8 = part.toUtf8();
        sha256UpdateBytes(state, QByteArrayView{utf8});
    } else if constexpr (std::same_as<Value, std::string>
                         || std::same_as<Value, std::string_view>) {
        sha256UpdateBytes(state, part.data(), part.size());
    } else if constexpr (ByteSpan<Value>) {
        sha256UpdateBytes(state, part.data(), part.size_bytes());
    }
}

} // namespace detail

template <detail::Sha256Part... Parts>
QByteArray sha256(const Parts&... parts)
{
    QByteArray digest(crypto_hash_sha256_BYTES, Qt::Uninitialized);
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    (detail::sha256Update(state, parts), ...);
    crypto_hash_sha256_final(&state,
                             reinterpret_cast<unsigned char *>(digest.data()));
    return digest;
}


// Initializes sodium library. We need one instance in  main().
class Crypto
{
public:
    Crypto();
    ~Crypto() = default;

    static QByteArray getHmacSha256(const QByteArray& key, std::initializer_list<const QByteArray*> data);
    static QByteArray getSha256(const QByteArray& data);

    template <detail::Sha256Part... Parts>
    static QByteArray getSha256(const Parts&... parts)
    {
        return sha256(parts...);
    }
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
