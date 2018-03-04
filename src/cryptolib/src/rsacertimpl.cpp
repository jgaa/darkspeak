
#include <assert.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/buffer.h>
#include <openssl/pem.h>

#include "ds/cvar.h"
#include "ds/rsacertimpl.h"

namespace ds {
namespace crypto {

RsaCertImpl::RsaCertImpl(const size_t bits)
    : bits_{bits}
{
    auto ctx = make_cvar(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL),
                         EVP_PKEY_CTX_free, "EVP_PKEY_CTX_new");

    if (!EVP_PKEY_keygen_init(ctx)) {
        throw Error("EVP_PKEY_keygen_init() failed");
    }

    if(!EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, static_cast<int>(bits))) {
        throw Error("EVP_PKEY_keygen_init() failed");
    }

    if (!EVP_PKEY_keygen(ctx, &key_)) {
        throw Error("EVP_PKEY_keygen() failed");
    }
}

RsaCertImpl::RsaCertImpl(const QByteArray &cert)
{
    auto bio = make_cvar(BIO_new_mem_buf(cert.data(),
                                         cert.length()),
                         BIO_free_all, "BIO_new_mem_buf");

    auto key = make_cvar(PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL),
                         EVP_PKEY_free, "PEM_read_bio_PUBKEY");

    auto rsa = EVP_PKEY_get1_RSA(key);
    if (!rsa) {
        throw Error("EVP_PKEY_get1_RSA");
    }


    auto evp_key = make_cvar(EVP_PKEY_new(), EVP_PKEY_free, "EVP_PKEY_new");

    if (!EVP_PKEY_set1_RSA(evp_key, rsa)) {
        throw Error("EVP_PKEY_set_RSA");
    }

    key_ = evp_key.take();

    if (!key_) {
        throw Error("EVP_PKEY_set_RSA did not set the RSA key");
    }
}

RsaCertImpl::~RsaCertImpl()
{
    if (key_) {
        EVP_PKEY_free(key_);
    }
}

QByteArray RsaCertImpl::getCert() const
{
    return getCertImpl([](BIO *bio, RSA *rsa) -> int {
        return PEM_write_bio_RSAPrivateKey(bio, rsa, NULL, NULL, 0, NULL, NULL);
    });
}

QByteArray RsaCertImpl::getPubKey() const
{
    return getCertImpl(PEM_write_bio_RSAPublicKey);
}

QByteArray RsaCertImpl::getCertImpl(std::function<int (BIO *, RSA *)> fn) const
{
    auto bio = make_cvar(BIO_new(BIO_s_mem()), BIO_free, "BIO_new");
    auto rsa = EVP_PKEY_get0_RSA(key_);
    if (!rsa) {
        throw Error("EVP_PKEY_get0_RSA");
    }

    fn(bio, rsa);
    BUF_MEM *mem = {};
    BIO_get_mem_ptr(bio, &mem);
    if (!mem) {
        throw Error("BIO_get_mem_ptr");
    }
    return {mem->data, static_cast<int>(mem->length)};
}

DsCert::ptr_t DsCert::create(const Type type) {
    size_t bits = {};

    switch(type) {
    case Type::RSA_512:
        bits = 512;
        break;
    case Type::RSA_1024:
        bits = 1024;
        break;
    case Type::RSA_2048:
        bits = 2048;
        break;
    case Type::RSA_4096:
        bits = 4096;
        break;
    }

    assert(bits != 0);

    return std::make_shared<RsaCertImpl>(bits);
}

DsCert::ptr_t DsCert::create(const QByteArray& cert) {
    return std::make_shared<RsaCertImpl>(cert);
}


}} // namespaces
