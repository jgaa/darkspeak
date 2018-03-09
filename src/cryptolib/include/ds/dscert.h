#ifndef DSCERT_H
#define DSCERT_H

#include <QByteArray>
#include <memory>

namespace ds {
namespace crypto {

/*! Interface for a PKI 'cert' used by one  Identity.
 */
class DsCert {
public:
    enum class Type {
        RSA_512, // For unit testing
        RSA_1024,
        RSA_2048,
        RSA_4096
    };

    struct Error : public std::runtime_error
    {
        Error(const char *what) : std::runtime_error(what) {}
    };

    using ptr_t = std::shared_ptr<DsCert>;

    virtual ~DsCert() = default;

    virtual QByteArray getCert() const = 0;
    virtual QByteArray getPubKey() const = 0;
    virtual QByteArray getHash() const = 0;

    /*! Factory to create a cert */
    static ptr_t create(const Type type = Type::RSA_2048);
    static ptr_t create(const QByteArray& cert);
    static ptr_t createFromPubkey(const QByteArray& pubkey); // as returned by getPubKey()
};

}} // namespaces

#endif // DSCERT_H
