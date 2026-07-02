#include "Crypto.h"
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/bio.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <memory>

struct MallocDeleter{void operator()(void*p)const{std::free(p);}};
template<typename T> struct MallocPtr : std::unique_ptr<T, MallocDeleter> {
    MallocPtr(std::size_t size)
	: std::unique_ptr<T, MallocDeleter>(static_cast<T*>(std::malloc(size)), MallocDeleter{})
    { if(!std::unique_ptr<T, MallocDeleter>::get()) throw std::bad_alloc(); }
};

namespace ha::crypto {

#define H(n,t,f) \
struct n##Free { void operator()(t* p) const { if(p) f(p); } }; \
using n##Ptr = std::unique_ptr<t, n##Free>;

H(Bio, BIO, BIO_free);
H(EvpPkey, EVP_PKEY, EVP_PKEY_free);
H(X509, X509, X509_free);

std::string b64(const unsigned char* data, int len) {
    BioPtr b64bio(BIO_new(BIO_f_base64()));
    if(!b64bio.get()) throw std::bad_alloc();
    BIO* mem = BIO_new(BIO_s_mem());
    if(!mem) throw std::bad_alloc();
    BIO_set_flags(b64bio.get(), BIO_FLAGS_BASE64_NO_NL);
    BIO* chain = BIO_push(b64bio.get(), mem);
    if(!chain) throw std::bad_alloc();
    BIO_write(chain, data, len);
    BIO_flush(chain);
    char* p = nullptr;
    long n = BIO_get_mem_data(mem, &p);
    return std::string(p, n);
}

void generateSelfSigned(const std::string& cn,
                        const std::string& keyPemPath,
                        const std::string& certPemPath) {
    // ключ EC P-256
    EvpPkeyPtr pkey(EVP_EC_gen("P-256"));
    if(!pkey.get()) throw std::runtime_error("EVP_EC_gen failed");

    X509Ptr x509(X509_new());
    if(!x509.get()) throw std::bad_alloc();
    ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1);
    X509_gmtime_adj(X509_get_notBefore(x509.get()), 0);
    X509_gmtime_adj(X509_get_notAfter(x509.get()), 60L * 60 * 24 * 36500); // 100 лет
    X509_set_pubkey(x509.get(), pkey.get());

    X509_NAME* name = X509_get_subject_name(x509.get());
    if(!name) throw std::bad_alloc();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)cn.c_str(), -1, -1, 0);
    X509_set_issuer_name(x509.get(), name);
    X509_sign(x509.get(), pkey.get(), EVP_sha256());

    FILE* kf = fopen(keyPemPath.c_str(), "wb");
    if (!kf) throw std::runtime_error("open key file");
    PEM_write_PrivateKey(kf, pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
    fclose(kf);

    FILE* cf = fopen(certPemPath.c_str(), "wb");
    if (!cf) throw std::runtime_error("open cert file");
    PEM_write_X509(cf, x509.get());
    fclose(cf);
}

static std::string spkiB64(X509* x509) {
    EvpPkeyPtr pub(X509_get_pubkey(x509));
    if (!pub.get()) throw std::runtime_error("no pubkey in cert");
    int len = i2d_PUBKEY(pub.get(), nullptr);
    MallocPtr<unsigned char> der(len);
    unsigned char* p = der.get();
    i2d_PUBKEY(pub.get(), &p);
    return b64(der.get(), len);
}

std::string publicKeyFromCert(const std::string& certPemPath) {
    FILE* cf = fopen(certPemPath.c_str(), "rb");
    if (!cf) throw std::runtime_error("open cert");
    X509Ptr x509(PEM_read_X509(cf, nullptr, nullptr, nullptr));
    fclose(cf);
    if (!x509.get()) throw std::runtime_error("parse cert");
    return spkiB64(x509.get());
}

std::string publicKeyFromCertPem(const std::string& certPem) {
    BioPtr bio(BIO_new_mem_buf(certPem.data(), (int)certPem.size()));
    if(!bio.get()) throw std::bad_alloc();
    X509Ptr x509(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!x509.get()) throw std::runtime_error("parse cert pem");
    return spkiB64(x509.get());
}

} // namespace ha::crypto
