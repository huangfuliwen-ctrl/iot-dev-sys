#include "tls_manager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <vector>
#include <mutex>
#include <filesystem>

#ifdef HAS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#endif

namespace dev_sys {

namespace {
    std::mutex g_cred_mutex;

    // Simple file read
    std::string read_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return "";
        std::ostringstream oss;
        oss << in.rdbuf();
        return oss.str();
    }

    // Simple file write
    bool write_file(const std::string& path, const std::string& data) {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;
        out.write(data.data(), data.size());
        return out.good();
    }
}

struct TlsManager::Impl {
    std::string ca_cert_path;
    std::string device_cert_path;
    std::string device_key_path;
    std::string ca_cert_pem;
    std::string device_cert_pem;
    std::string device_key_pem;
    bool initialized = false;

#ifdef HAS_OPENSSL
    SSL_CTX* ssl_ctx = nullptr;
    X509* device_x509 = nullptr;
    EVP_PKEY* device_pkey = nullptr;
#endif

    void cleanup() {
#ifdef HAS_OPENSSL
        if (device_x509) { X509_free(device_x509); device_x509 = nullptr; }
        if (device_pkey) { EVP_PKEY_free(device_pkey); device_pkey = nullptr; }
        if (ssl_ctx)     { SSL_CTX_free(ssl_ctx); ssl_ctx = nullptr; }
#endif
    }
};

TlsManager::TlsManager()
    : impl_(std::make_unique<Impl>()) {}

TlsManager::~TlsManager() {
    impl_->cleanup();
}

StatusCode TlsManager::init(const std::string& ca_cert_path,
                             const std::string& device_cert_path,
                             const std::string& device_key_path) {
    impl_->ca_cert_path     = ca_cert_path;
    impl_->device_cert_path = device_cert_path;
    impl_->device_key_path  = device_key_path;

    // Load PEM files into memory
    impl_->ca_cert_pem     = read_file(ca_cert_path);
    impl_->device_cert_pem = read_file(device_cert_path);
    impl_->device_key_pem  = read_file(device_key_path);

#ifdef HAS_OPENSSL
    // Initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Create SSL context (TLS client method — used for outbound MQTT & HTTP connections)
    const SSL_METHOD* method = TLS_client_method();
    impl_->ssl_ctx = SSL_CTX_new(method);
    if (!impl_->ssl_ctx) {
        std::cerr << "[TlsManager] SSL_CTX_new failed" << std::endl;
        ERR_print_errors_fp(stderr);
        return StatusCode::COMM_TLS_FAILED;
    }

    // Enforce TLS 1.2+
    SSL_CTX_set_min_proto_version(impl_->ssl_ctx, TLS1_2_VERSION);

    // Load CA certificate
    if (!impl_->ca_cert_pem.empty()) {
        BIO* ca_bio = BIO_new_mem_buf(impl_->ca_cert_pem.data(),
                                       static_cast<int>(impl_->ca_cert_pem.size()));
        if (!ca_bio) {
            std::cerr << "[TlsManager] BIO_new_mem_buf failed for CA cert" << std::endl;
            return StatusCode::COMM_TLS_FAILED;
        }
        X509* ca_cert = PEM_read_bio_X509(ca_bio, nullptr, nullptr, nullptr);
        BIO_free(ca_bio);
        if (!ca_cert) {
            std::cerr << "[TlsManager] Failed to parse CA certificate" << std::endl;
            ERR_print_errors_fp(stderr);
            return StatusCode::DEV_CERT_INVALID;
        }
        X509_STORE* store = SSL_CTX_get_cert_store(impl_->ssl_ctx);
        X509_STORE_add_cert(store, ca_cert);
        X509_free(ca_cert);
    }

    // Load device certificate
    if (!impl_->device_cert_pem.empty()) {
        BIO* cert_bio = BIO_new_mem_buf(impl_->device_cert_pem.data(),
                                         static_cast<int>(impl_->device_cert_pem.size()));
        if (!cert_bio) {
            std::cerr << "[TlsManager] BIO_new_mem_buf failed for device cert" << std::endl;
            return StatusCode::COMM_TLS_FAILED;
        }
        impl_->device_x509 = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        BIO_free(cert_bio);
        if (!impl_->device_x509) {
            std::cerr << "[TlsManager] Failed to parse device certificate" << std::endl;
            ERR_print_errors_fp(stderr);
            return StatusCode::DEV_CERT_INVALID;
        }
        if (SSL_CTX_use_certificate(impl_->ssl_ctx, impl_->device_x509) != 1) {
            std::cerr << "[TlsManager] SSL_CTX_use_certificate failed" << std::endl;
            return StatusCode::COMM_TLS_FAILED;
        }
    }

    // Load device private key
    if (!impl_->device_key_pem.empty()) {
        BIO* key_bio = BIO_new_mem_buf(impl_->device_key_pem.data(),
                                        static_cast<int>(impl_->device_key_pem.size()));
        if (!key_bio) {
            std::cerr << "[TlsManager] BIO_new_mem_buf failed for device key" << std::endl;
            return StatusCode::COMM_TLS_FAILED;
        }
        impl_->device_pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        if (!impl_->device_pkey) {
            std::cerr << "[TlsManager] Failed to parse device private key" << std::endl;
            ERR_print_errors_fp(stderr);
            return StatusCode::COMM_TLS_FAILED;
        }
        if (SSL_CTX_use_PrivateKey(impl_->ssl_ctx, impl_->device_pkey) != 1) {
            std::cerr << "[TlsManager] SSL_CTX_use_PrivateKey failed" << std::endl;
            return StatusCode::COMM_TLS_FAILED;
        }
        // Verify private key matches certificate
        if (SSL_CTX_check_private_key(impl_->ssl_ctx) != 1) {
            std::cerr << "[TlsManager] Private key does not match certificate" << std::endl;
            return StatusCode::COMM_TLS_FAILED;
        }
    }

    impl_->initialized = true;
    return StatusCode::OK;
#else
    // Stub mode: mark initialized even without OpenSSL
    impl_->initialized = true;
    std::cerr << "[TlsManager] WARNING: Built without OpenSSL. TLS is NOT available." << std::endl;
    return StatusCode::OK;
#endif
}

bool TlsManager::is_cert_valid() const {
    if (!impl_->initialized) return false;

#ifdef HAS_OPENSSL
    if (!impl_->device_x509) return false;

    // Check notBefore and notAfter
    time_t now = time(nullptr);
    int day_before = X509_cmp_time(X509_get0_notBefore(impl_->device_x509), &now);
    int day_after  = X509_cmp_time(X509_get0_notAfter(impl_->device_x509), &now);

    // day_before should be <= 0 (now is after or at notBefore)
    // day_after should be >= 0 (now is before or at notAfter)
    if (day_before > 0) {
        std::cerr << "[TlsManager] Certificate not yet valid" << std::endl;
        return false;
    }
    if (day_after < 0) {
        std::cerr << "[TlsManager] Certificate expired" << std::endl;
        return false;
    }
    return true;
#else
    return impl_->initialized;
#endif
}

int TlsManager::cert_days_until_expiry() const {
    if (!impl_->initialized) return -1;

#ifdef HAS_OPENSSL
    if (!impl_->device_x509) return -1;

    const ASN1_TIME* notAfter = X509_get0_notAfter(impl_->device_x509);
    if (!notAfter) return -1;

    int days, seconds;
    if (ASN1_TIME_diff(&days, &seconds, nullptr, notAfter)) {
        return days;
    }
    return -1;
#else
    return 365; // stub
#endif
}

StatusCode TlsManager::request_cert_renewal() {
#ifdef HAS_OPENSSL
    // Generate a new RSA key pair
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) return StatusCode::ERROR;

    EVP_PKEY* new_key = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0 ||
        EVP_PKEY_keygen(pctx, &new_key) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return StatusCode::ERROR;
    }
    EVP_PKEY_CTX_free(pctx);

    // Create X509 certificate signing request (CSR)
    X509_REQ* req = X509_REQ_new();
    if (!req) {
        EVP_PKEY_free(new_key);
        return StatusCode::ERROR;
    }

    X509_REQ_set_pubkey(req, new_key);
    X509_REQ_set_version(req, 0);

    // Set subject (CN = device ID placeholder)
    X509_NAME* name = X509_REQ_get_subject_name(req);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                (const unsigned char*)"iot-device", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                (const unsigned char*)"IoT Platform", -1, -1, 0);

    // Sign the CSR
    if (X509_REQ_sign(req, new_key, EVP_sha256()) <= 0) {
        X509_REQ_free(req);
        EVP_PKEY_free(new_key);
        return StatusCode::ERROR;
    }

    // Export CSR as PEM
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(bio, req);
    char* pem_data = nullptr;
    long pem_len = BIO_get_mem_data(bio, &pem_data);
    std::string csr_pem(pem_data, pem_len);

    // Export new private key as PEM
    BIO* key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(key_bio, new_key, nullptr, nullptr, 0, nullptr, nullptr);
    char* key_data = nullptr;
    long key_len = BIO_get_mem_data(key_bio, &key_data);
    std::string key_pem(key_data, key_len);

    // Store the new key in secure storage (will be used after CA signs the CSR)
    store_credential("pending_key", key_pem);
    store_credential("pending_csr", csr_pem);

    // Cleanup
    BIO_free(key_bio);
    BIO_free(bio);
    X509_REQ_free(req);
    EVP_PKEY_free(new_key);

    std::cout << "[TlsManager] CSR generated and stored. csr_size=" << csr_pem.size() << std::endl;
    return StatusCode::OK;
#else
    std::cerr << "[TlsManager] Certificate renewal requires OpenSSL" << std::endl;
    return StatusCode::ERROR;
#endif
}

StatusCode TlsManager::store_credential(const std::string& key,
                                         const std::string& value) {
    std::lock_guard<std::mutex> lock(g_cred_mutex);

    // Determine secure storage path
    std::string cred_dir = "/etc/dev-sys-cloud/credentials";
    std::error_code ec;
    if (!std::filesystem::exists(cred_dir, ec)) {
        std::filesystem::create_directories(cred_dir, ec);
        if (ec) {
            std::cerr << "[TlsManager] Failed to create credential dir: " << cred_dir << std::endl;
            return StatusCode::STORAGE_WRITE_ERROR;
        }
        // Set restrictive permissions on credential directory
        std::filesystem::permissions(cred_dir,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace, ec);
    }

    std::string file_path = cred_dir + "/" + key + ".cred";

#ifdef HAS_OPENSSL
    // Encrypt with AES-256-GCM using a simple key derivation
    // In production, use hardware-backed key storage (TPM/TEE)
    unsigned char iv[12] = {};
    unsigned char enc_key[32] = {};
    // Simple KDF: hash key name with fixed seed (production: use TPM or HSM)
    std::string kdf_input = "dev-sys-cred-v1:" + key;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(mdctx, kdf_input.data(), kdf_input.size());
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    memcpy(enc_key, hash, 32);
    // Use first 12 bytes of hash as IV (deterministic per key)
    memcpy(iv, hash, 12);

    // Encrypt
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return StatusCode::ERROR;

    std::vector<unsigned char> ciphertext(value.size() + 16);
    int out_len = 0, final_len = 0;

    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, enc_key, iv);
    EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len,
                      reinterpret_cast<const unsigned char*>(value.data()),
                      static_cast<int>(value.size()));
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len, &final_len);
    out_len += final_len;

    // Get GCM tag
    unsigned char tag[16];
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);

    // Write: IV (12 bytes) + tag (16 bytes) + ciphertext
    std::string file_data(reinterpret_cast<char*>(iv), 12);
    file_data.append(reinterpret_cast<char*>(tag), 16);
    file_data.append(reinterpret_cast<char*>(ciphertext.data()), out_len);

    if (!write_file(file_path, file_data)) {
        return StatusCode::STORAGE_WRITE_ERROR;
    }
#else
    // No encryption available — store as plaintext (INSECURE, development only)
    if (!write_file(file_path, value)) {
        return StatusCode::STORAGE_WRITE_ERROR;
    }
#endif

    // Restrict file permissions to owner-only
    std::filesystem::permissions(file_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);

    return StatusCode::OK;
}

StatusCode TlsManager::load_credential(const std::string& key,
                                        std::string& value) {
    std::lock_guard<std::mutex> lock(g_cred_mutex);

    std::string file_path = "/etc/dev-sys-cloud/credentials/" + key + ".cred";
    std::string file_data = read_file(file_path);
    if (file_data.empty()) {
        return StatusCode::STORAGE_READ_ERROR;
    }

#ifdef HAS_OPENSSL
    // Decrypt
    if (file_data.size() < 28) { // IV(12) + Tag(16) = 28 bytes minimum
        std::cerr << "[TlsManager] Credential file too small: " << file_path << std::endl;
        return StatusCode::STORAGE_READ_ERROR;
    }

    unsigned char iv[12];
    unsigned char tag[16];
    memcpy(iv, file_data.data(), 12);
    memcpy(tag, file_data.data() + 12, 16);

    const unsigned char* ciphertext = reinterpret_cast<const unsigned char*>(
        file_data.data() + 28);
    int ciphertext_len = static_cast<int>(file_data.size()) - 28;

    // Derive key (same KDF as store)
    unsigned char enc_key[32];
    std::string kdf_input = "dev-sys-cred-v1:" + key;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(mdctx, kdf_input.data(), kdf_input.size());
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    memcpy(enc_key, hash, 32);

    // Decrypt
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return StatusCode::ERROR;

    std::vector<unsigned char> plaintext(ciphertext_len + 16);
    int out_len = 0, final_len = 0;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, enc_key, iv);
    EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, ciphertext, ciphertext_len);

    // Set expected tag
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag);

    int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) {
        std::cerr << "[TlsManager] Credential decryption failed (tag mismatch)" << std::endl;
        return StatusCode::STORAGE_READ_ERROR;
    }

    value.assign(reinterpret_cast<char*>(plaintext.data()), out_len + final_len);
#else
    value = file_data; // plaintext
#endif

    return StatusCode::OK;
}

} // namespace dev_sys
