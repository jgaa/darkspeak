#ifndef DS_HANDSHAKE_H
#define DS_HANDSHAKE_H

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>

#include <QByteArray>
#include <sodium.h>

#include "ds/dscert.h"

namespace ds::prot::handshake {

inline constexpr size_t maxIncompleteHandshakes = 64;
inline std::atomic<size_t> incompleteHandshakes = 0;

// Returns an aggregate count only when a warning should be emitted. Callers
// must keep connection-specific details at debug level.
inline std::optional<size_t> recordFloodEvent()
{
    static std::mutex mutex;
    static size_t pending = 0;
    static auto lastWarning = std::chrono::steady_clock::now()
            - std::chrono::seconds{10};
    std::lock_guard lock{mutex};
    ++pending;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastWarning < std::chrono::seconds{10}) {
        return std::nullopt;
    }
    const auto count = pending;
    pending = 0;
    lastWarning = now;
    return count;
}

class BudgetLease final {
public:
    BudgetLease() = default;
    BudgetLease(const BudgetLease&) = delete;
    BudgetLease& operator=(const BudgetLease&) = delete;
    ~BudgetLease() { release(); }

    bool acquire() noexcept
    {
        size_t current = incompleteHandshakes.load(std::memory_order_relaxed);
        while (current < maxIncompleteHandshakes
               && !incompleteHandshakes.compare_exchange_weak(
                   current, current + 1, std::memory_order_acquire,
                   std::memory_order_relaxed)) {
        }
        if (current >= maxIncompleteHandshakes) {
            return false;
        }
        held_ = true;
        return true;
    }

    void release() noexcept
    {
        if (held_) {
            incompleteHandshakes.fetch_sub(1, std::memory_order_release);
            held_ = false;
        }
    }

private:
    bool held_ = false;
};

inline constexpr unsigned char version = 2;
inline constexpr size_t clientHelloBytes = 1 + crypto_kx_PUBLICKEYBYTES
        + 32 + crypto_sign_PUBLICKEYBYTES + crypto_sign_BYTES;
inline constexpr size_t serverHelloBytes = 1 + crypto_kx_PUBLICKEYBYTES
        + 32 + crypto_secretstream_xchacha20poly1305_HEADERBYTES + crypto_sign_BYTES;
inline constexpr size_t clientFinishBytes = 1
        + crypto_secretstream_xchacha20poly1305_HEADERBYTES
        + crypto_sign_BYTES + crypto_auth_BYTES;

inline QByteArray domain(const char *value)
{
    return QByteArray{value};
}

inline QByteArray sha256(const QByteArray& value)
{
    QByteArray result(crypto_hash_sha256_BYTES, Qt::Uninitialized);
    crypto_hash_sha256(reinterpret_cast<unsigned char *>(result.data()),
                       reinterpret_cast<const unsigned char *>(value.constData()),
                       static_cast<unsigned long long>(value.size()));
    return result;
}

inline QByteArray sign(const crypto::DsCert& cert, const QByteArray& input)
{
    if (cert.getSigningKey().size() != crypto_sign_SECRETKEYBYTES) {
        throw std::runtime_error("Invalid handshake signing key");
    }
    QByteArray result(crypto_sign_BYTES, Qt::Uninitialized);
    unsigned long long length = {};
    if (crypto_sign_detached(reinterpret_cast<unsigned char *>(result.data()), &length,
                             reinterpret_cast<const unsigned char *>(input.constData()),
                             static_cast<unsigned long long>(input.size()),
                             cert.getSigningKey().cdata()) != 0
            || length != crypto_sign_BYTES) {
        throw std::runtime_error("Failed to sign handshake");
    }
    return result;
}

inline bool verify(const QByteArray& publicKey, const QByteArray& signature,
                   const QByteArray& input)
{
    return publicKey.size() == crypto_sign_PUBLICKEYBYTES
            && signature.size() == crypto_sign_BYTES
            && crypto_sign_verify_detached(
                reinterpret_cast<const unsigned char *>(signature.constData()),
                reinterpret_cast<const unsigned char *>(input.constData()),
                static_cast<unsigned long long>(input.size()),
                reinterpret_cast<const unsigned char *>(publicKey.constData())) == 0;
}

inline void randomBytes(QByteArray& value)
{
    randombytes_buf(value.data(), static_cast<size_t>(value.size()));
}

inline QByteArray signedInput(const char *label, const QByteArray& fields)
{
    QByteArray result = domain(label);
    result += fields;
    return result;
}

} // namespace ds::prot::handshake

#endif // DS_HANDSHAKE_H
