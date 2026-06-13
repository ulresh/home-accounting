#include "Crypto.h"
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/bio.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <vector>
#include <memory>

namespace ha::crypto {

namespace {
struct BioFree { void operator()(BIO* b) const { if (b) BIO_free(b); } };
using BioPtr = std::unique_ptr<BIO, BioFree>;

std::string b64(const unsigned char* data, int len) {
    BioPtr b64bio(BIO_new(BIO_f_base64()));
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64bio.get(), BIO_FLAGS_BASE64_NO_NL);
    BIO* chain = BIO_push(b64bio.get(), mem);
    BIO_write(chain, data, len);
    BIO_flush(chain);
    char* p = nullptr;
    long n = BIO_get_mem_data(mem, &p);
    std::string out(p, n);
    return out;
}
} // namespace

void generateSelfSigned(const std::string& cn,
                        const std::string& keyPemPath,
                        const std::string& certPemPath) {
    // ключ EC P-256
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) throw std::runtime_error("EVP_EC_gen failed");

    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 60L * 60 * 24 * 3650); // 10 лет
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)cn.c_str(), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    FILE* kf = fopen(keyPemPath.c_str(), "wb");
    if (!kf) throw std::runtime_error("open key file");
    PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(kf);

    FILE* cf = fopen(certPemPath.c_str(), "wb");
    if (!cf) throw std::runtime_error("open cert file");
    PEM_write_X509(cf, x509);
    fclose(cf);

    X509_free(x509);
    EVP_PKEY_free(pkey);
}

static std::string spkiB64(X509* x509) {
    EVP_PKEY* pub = X509_get_pubkey(x509);
    if (!pub) throw std::runtime_error("no pubkey in cert");
    int len = i2d_PUBKEY(pub, nullptr);
    std::vector<unsigned char> der(len);
    unsigned char* p = der.data();
    i2d_PUBKEY(pub, &p);
    EVP_PKEY_free(pub);
    return b64(der.data(), len);
}

std::string publicKeyFromCert(const std::string& certPemPath) {
    FILE* cf = fopen(certPemPath.c_str(), "rb");
    if (!cf) throw std::runtime_error("open cert");
    X509* x509 = PEM_read_X509(cf, nullptr, nullptr, nullptr);
    fclose(cf);
    if (!x509) throw std::runtime_error("parse cert");
    std::string out = spkiB64(x509);
    X509_free(x509);
    return out;
}

std::string publicKeyFromCertPem(const std::string& certPem) {
    BioPtr bio(BIO_new_mem_buf(certPem.data(), (int)certPem.size()));
    X509* x509 = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
    if (!x509) throw std::runtime_error("parse cert pem");
    std::string out = spkiB64(x509);
    X509_free(x509);
    return out;
}

} // namespace ha::crypto
