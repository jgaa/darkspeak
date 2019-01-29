#ifndef DSCERT_H
#define DSCERT_H

#include <ds/safememory.h>
#include <memory>
#include <QByteArray>

#include "ds/safememory.h"
#include "ds/memoryview.h"

#include "base58.h"

namespace ds {
namespace crypto {

/*! Interface for a PKI 'cert' used by one  Identity.
 */
class DsCert {
public:
    using safe_array_t = SafeMemory<std::uint8_t>;
    using safe_view_t = MemoryView<std::uint8_t>;

    struct Error : public std::runtime_error
    {
        Error(const char *what) : std::runtime_error(what) {}
    };

    using ptr_t = std::shared_ptr<DsCert>;

    virtual ~DsCert() = default;

    virtual const safe_array_t& getCert() const = 0;
    virtual const safe_view_t& getKey() const = 0;
    virtual const safe_view_t& getPubKey() const = 0;
    virtual const safe_array_t& getHash() const = 0;
    virtual const QByteArray getB58PubKey() {
        return b58check_enc<QByteArray>(getPubKey(), {249, 50});
    }

    template <typename Tsign, typename T>
    Tsign sign(std::initializer_list<T> data) const
    {
        assert(!getKey().empty());

        Tsign signature;
        signature.resize(crypto_sign_BYTES);

        crypto_sign_state state = {};
        crypto_sign_init(&state);
        for(const auto& d : data) {
            crypto_sign_update(&state,
                               reinterpret_cast<const unsigned char *>(d.data()),
                               static_cast<size_t>(d.size()));

        }
        crypto_sign_final_create(&state,
                                 reinterpret_cast<unsigned char *>(signature.data()),
                                 nullptr, getKey().cdata());
        return signature;
    }

    template <typename T, typename Tsign>
    bool verify(const Tsign &signature, std::initializer_list<T> data) const
    {
        assert(!getKey().empty());
        crypto_sign_state state = {};
        crypto_sign_init(&state);
        for(const auto& d : data) {
            crypto_sign_update(&state,
                               reinterpret_cast<const unsigned char *>(d.cdata()),
                               d.size());
        }
        Tsign sign = signature; // libsodium wants non-const signature
        const auto result = crypto_sign_final_verify(&state,
                                                     sign.data(), getPubKey().cdata());
        return result == 0;
    }

//    // Sign data with the private key
//    virtual safe_array_t sign(std::initializer_list<const safe_view_t> data) const = 0;

//    // Verify a signature using the public key
//    virtual bool verify(const safe_array_t& signature,
//                        std::initializer_list<const safe_view_t> data) const = 0;

    /*! Factory to create a cert */
    static ptr_t create();
    static ptr_t create(const safe_array_t& cert);
    static ptr_t createFromPubkey(const safe_array_t& pubkey); // as returned by getPubKey()
    static ptr_t createFromPubkey(const QByteArray& pubkey);
};

}} // namespaces

#endif // DSCERT_H
