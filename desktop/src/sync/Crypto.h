#pragma once
#include <string>

namespace ha::crypto {

// Сгенерировать самоподписанный сертификат + ключ (EC P-256) и сохранить в PEM.
void generateSelfSigned(const std::string& cn,
                        const std::string& keyPemPath,
                        const std::string& certPemPath);

// Публичный ключ из сертификата как идентификатор устройства:
// DER SubjectPublicKeyInfo в base64 одной строкой.
std::string publicKeyFromCert(const std::string& certPemPath);

// То же, но из объекта сертификата по пути в памяти (PEM-строка).
std::string publicKeyFromCertPem(const std::string& certPem);
std::string peerPubkey(const void *ssl);

} // namespace ha::crypto
