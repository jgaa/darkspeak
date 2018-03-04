#ifndef CRYPTO_H
#define CRYPTO_H

namespace ds {
namespace crypto {


// Initializes openssl library. We need one instance in  main().
class Crypto
{
public:
    Crypto();
    ~Crypto();
};

}} // namespaces

#endif // CRYPTO_H
