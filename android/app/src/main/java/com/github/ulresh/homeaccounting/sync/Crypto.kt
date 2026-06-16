package com.github.ulresh.homeaccounting.sync

import org.bouncycastle.asn1.x500.X500Name
import org.bouncycastle.cert.jcajce.JcaX509CertificateConverter
import org.bouncycastle.cert.jcajce.JcaX509v3CertificateBuilder
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder
import java.io.File
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.SecureRandom
import java.security.cert.Certificate
import java.security.cert.X509Certificate
import java.security.spec.ECGenParameterSpec
import java.util.Base64
import java.util.Date
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

// Идентичность устройства: самоподписанный сертификат (EC P-256).
// Идентификатор = DER SubjectPublicKeyInfo в base64 (как на Desktop).
object Crypto {
    private const val ALIAS = "device"
    private val PW = "homeaccounting".toCharArray()

    // Создать (при первом запуске) или загрузить ключ/сертификат.
    // Возвращает (pubkey, keyStore). Состояние НЕ хранится в объекте, чтобы в одном
    // процессе могли сосуществовать несколько независимых хранилищ (в т.ч. в тестах).
    fun ensureIdentity(dir: File): Pair<String, KeyStore> {
        val ksFile = File(dir, "identity.p12")
        val ks = KeyStore.getInstance("PKCS12")
        if (ksFile.exists()) {
            ksFile.inputStream().use { ks.load(it, PW) }
        } else {
            ks.load(null, null)
            val kpg = KeyPairGenerator.getInstance("EC")
            kpg.initialize(ECGenParameterSpec("secp256r1"))
            val kp = kpg.generateKeyPair()

            val now = Date()
            val notAfter = Date(now.time + 3650L * 24 * 60 * 60 * 1000)
            val name = X500Name("CN=DomUchet-Device")
            val builder = JcaX509v3CertificateBuilder(
                name, BigInteger.valueOf(System.currentTimeMillis()),
                now, notAfter, name, kp.public
            )
            val signer = JcaContentSignerBuilder("SHA256withECDSA").build(kp.private)
            val cert: X509Certificate = JcaX509CertificateConverter().getCertificate(builder.build(signer))

            ks.setKeyEntry(ALIAS, kp.private, PW, arrayOf<Certificate>(cert))
            dir.mkdirs()
            ksFile.outputStream().use { ks.store(it, PW) }
        }
        val cert = ks.getCertificate(ALIAS)
        return pubkeyOf(cert) to ks
    }

    fun pubkeyOf(cert: Certificate): String =
        Base64.getEncoder().encodeToString(cert.publicKey.encoded)

    // SSLContext с нашим ключом и доверием ко всем (идентичность проверяется по pubkey).
    fun sslContext(ks: KeyStore): SSLContext {
        val kmf = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm())
        kmf.init(ks, PW)
        val trustAll = arrayOf<TrustManager>(object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
            override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
        })
        val ctx = SSLContext.getInstance("TLS")
        ctx.init(kmf.keyManagers, trustAll, SecureRandom())
        return ctx
    }
}
