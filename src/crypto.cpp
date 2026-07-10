#include "qrtp/crypto.hpp"
#include "qrtp/base64.hpp"

#include <stdexcept>

namespace qrtp {
namespace {
constexpr int kPbkdf2Iterations = 100000;
constexpr size_t kSaltLen = 16;
constexpr size_t kIvLen = 12;
constexpr size_t kKeyLen = 32;
constexpr size_t kTagLen = 16;
} // namespace
} // namespace qrtp

// ============================================================================================
#ifdef _WIN32
// Windows: bcrypt.dll (CNG) is part of Windows itself since Vista -- unlike qr_encoder.cpp's
// qrencode.dll (a genuinely optional third-party library, hence LoadLibrary there), this is
// linked directly, the same way gui_win32.cpp already links user32/gdi32 directly. Needs
// `-lbcrypt` on the link line (see Makefile/CMakeLists.txt) -- always present in a MinGW/MSVC
// Windows SDK, no extra install.
#include <windows.h>
#include <bcrypt.h>

namespace qrtp {
namespace {

inline bool bcrypt_ok(NTSTATUS s) { return s >= 0; } // same test as the standard NT_SUCCESS macro

struct AlgHandle {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct KeyHandle {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~KeyHandle() { if (h) BCryptDestroyKey(h); }
};

// Shared setup for both aes256gcm_encrypt/decrypt below: opens the AES provider, switches it to
// GCM chaining, and imports `key` as a symmetric key object.
void open_aes_gcm_key(const std::vector<uint8_t>& key, AlgHandle& alg, KeyHandle& out_key) {
    if (key.size() != kKeyLen) throw std::runtime_error("crypto: AES-256-GCM key must be 32 bytes");
    if (!bcrypt_ok(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_AES_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("crypto: BCryptOpenAlgorithmProvider(AES) failed");
    if (!bcrypt_ok(BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0)))
        throw std::runtime_error("crypto: setting BCRYPT_CHAIN_MODE_GCM failed");
    if (!bcrypt_ok(BCryptGenerateSymmetricKey(alg.h, &out_key.h, nullptr, 0,
                                                const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0)))
        throw std::runtime_error("crypto: BCryptGenerateSymmetricKey failed");
}

} // namespace

void random_bytes(uint8_t* buf, size_t len) {
    if (!bcrypt_ok(BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("crypto: BCryptGenRandom failed");
}

std::vector<uint8_t> pbkdf2_hmac_sha256(const std::string& passphrase, const std::vector<uint8_t>& salt,
                                         int iterations, size_t key_len) {
    AlgHandle alg;
    if (!bcrypt_ok(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        throw std::runtime_error("crypto: BCryptOpenAlgorithmProvider(SHA256/HMAC) failed");

    std::vector<uint8_t> out(key_len);
    NTSTATUS status = BCryptDeriveKeyPBKDF2(
        alg.h,
        reinterpret_cast<PUCHAR>(const_cast<char*>(passphrase.data())), static_cast<ULONG>(passphrase.size()),
        const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()),
        static_cast<ULONGLONG>(iterations),
        out.data(), static_cast<ULONG>(out.size()),
        0);
    if (!bcrypt_ok(status)) throw std::runtime_error("crypto: BCryptDeriveKeyPBKDF2 failed");
    return out;
}

std::vector<uint8_t> aes256gcm_encrypt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv,
                                       const std::vector<uint8_t>& plaintext) {
    if (iv.size() != kIvLen) throw std::runtime_error("crypto: AES-GCM iv must be 12 bytes");
    AlgHandle alg;
    KeyHandle bkey;
    open_aes_gcm_key(key, alg, bkey);

    std::vector<uint8_t> ciphertext(plaintext.size() + kTagLen); // headroom keeps .data() non-null even if empty
    uint8_t tag[kTagLen];

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv.data());
    authInfo.cbNonce = static_cast<ULONG>(iv.size());
    authInfo.pbTag = tag;
    authInfo.cbTag = sizeof(tag);

    ULONG out_len = 0;
    NTSTATUS status = BCryptEncrypt(bkey.h,
                                     const_cast<PUCHAR>(plaintext.data()), static_cast<ULONG>(plaintext.size()),
                                     &authInfo, nullptr, 0,
                                     ciphertext.data(), static_cast<ULONG>(ciphertext.size()), &out_len, 0);
    if (!bcrypt_ok(status)) throw std::runtime_error("crypto: BCryptEncrypt failed");
    ciphertext.resize(out_len);

    std::vector<uint8_t> out;
    out.reserve(ciphertext.size() + kTagLen);
    out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    out.insert(out.end(), tag, tag + kTagLen);
    return out;
}

std::vector<uint8_t> aes256gcm_decrypt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv,
                                       const std::vector<uint8_t>& ciphertext_and_tag) {
    if (iv.size() != kIvLen) throw std::runtime_error("crypto: AES-GCM iv must be 12 bytes");
    if (ciphertext_and_tag.size() < kTagLen) throw std::runtime_error("crypto: ciphertext too short to contain a tag");
    const size_t ct_len = ciphertext_and_tag.size() - kTagLen;

    AlgHandle alg;
    KeyHandle bkey;
    open_aes_gcm_key(key, alg, bkey);

    std::vector<uint8_t> plaintext(ct_len + kTagLen); // headroom, same nullptr-safety reasoning as encrypt

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv.data());
    authInfo.cbNonce = static_cast<ULONG>(iv.size());
    authInfo.pbTag = const_cast<PUCHAR>(ciphertext_and_tag.data() + ct_len);
    authInfo.cbTag = static_cast<ULONG>(kTagLen);

    ULONG out_len = 0;
    // BCryptDecrypt returns a failing NTSTATUS (STATUS_AUTH_TAG_MISMATCH specifically) when the
    // tag doesn't verify -- treated identically to any other failure: wrong passphrase and
    // tampered/foreign data must both just mean "can't decrypt," never partial plaintext.
    NTSTATUS status = BCryptDecrypt(bkey.h,
                                     const_cast<PUCHAR>(ciphertext_and_tag.data()), static_cast<ULONG>(ct_len),
                                     &authInfo, nullptr, 0,
                                     plaintext.data(), static_cast<ULONG>(plaintext.size()), &out_len, 0);
    if (!bcrypt_ok(status))
        throw std::runtime_error("crypto: authentication failed (wrong passphrase, or not a valid encrypted message)");
    plaintext.resize(out_len);
    return plaintext;
}

} // namespace qrtp

// ============================================================================================
#else
// Linux/macOS: OpenSSL's libcrypto, loaded at runtime with dlopen -- same "works against the
// plain runtime .so, no -devel/header package required" convention qr_encoder.cpp already uses
// for libqrencode. The small subset of EVP's C ABI needed here is hand-declared below (opaque
// pointers for EVP_MD/EVP_CIPHER/EVP_CIPHER_CTX) rather than including <openssl/evp.h>, so this
// keeps building even on a system that only has the runtime library installed.
#include <dlfcn.h>

namespace qrtp {
namespace {

// Long-stable public ABI constants from openssl/evp.h (GCM ctrl support since OpenSSL 1.0.1;
// changing these would break ABI for the entire ecosystem, so treated as safe to hand-declare).
constexpr int kEvpCtrlGcmSetIvLen = 0x9;
constexpr int kEvpCtrlGcmGetTag = 0x10;
constexpr int kEvpCtrlGcmSetTag = 0x11;

using EVP_sha256_fn = const void* (*)(void);
using EVP_aes_256_gcm_fn = const void* (*)(void);
using EVP_CIPHER_CTX_new_fn = void* (*)(void);
using EVP_CIPHER_CTX_free_fn = void (*)(void*);
using EVP_CIPHER_CTX_ctrl_fn = int (*)(void* ctx, int type, int arg, void* ptr);
using EVP_EncryptInit_ex_fn = int (*)(void* ctx, const void* cipher, void* impl, const unsigned char* key, const unsigned char* iv);
using EVP_EncryptUpdate_fn = int (*)(void* ctx, unsigned char* out, int* outl, const unsigned char* in, int inl);
using EVP_EncryptFinal_ex_fn = int (*)(void* ctx, unsigned char* out, int* outl);
using EVP_DecryptInit_ex_fn = int (*)(void* ctx, const void* cipher, void* impl, const unsigned char* key, const unsigned char* iv);
using EVP_DecryptUpdate_fn = int (*)(void* ctx, unsigned char* out, int* outl, const unsigned char* in, int inl);
using EVP_DecryptFinal_ex_fn = int (*)(void* ctx, unsigned char* out, int* outl);
using PKCS5_PBKDF2_HMAC_fn = int (*)(const char* pass, int passlen, const unsigned char* salt, int saltlen,
                                      int iter, const void* digest, int keylen, unsigned char* out);
using RAND_bytes_fn = int (*)(unsigned char* buf, int num);

struct LibCrypto {
    void* handle = nullptr;
    const void* sha256 = nullptr;
    const void* aes_256_gcm = nullptr;
    EVP_CIPHER_CTX_new_fn ctx_new = nullptr;
    EVP_CIPHER_CTX_free_fn ctx_free = nullptr;
    EVP_CIPHER_CTX_ctrl_fn ctx_ctrl = nullptr;
    EVP_EncryptInit_ex_fn encrypt_init = nullptr;
    EVP_EncryptUpdate_fn encrypt_update = nullptr;
    EVP_EncryptFinal_ex_fn encrypt_final = nullptr;
    EVP_DecryptInit_ex_fn decrypt_init = nullptr;
    EVP_DecryptUpdate_fn decrypt_update = nullptr;
    EVP_DecryptFinal_ex_fn decrypt_final = nullptr;
    PKCS5_PBKDF2_HMAC_fn pbkdf2 = nullptr;
    RAND_bytes_fn rand_bytes = nullptr;

    LibCrypto() {
        // SONAME varies by distro/OpenSSL version; try the common ones in order (newest first).
        for (const char* name : {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"}) {
            handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (handle) break;
        }
        if (!handle) throw std::runtime_error(std::string("crypto: cannot load libcrypto: ") + dlerror());

        auto sym = [&](const char* name) {
            void* p = dlsym(handle, name);
            if (!p) throw std::runtime_error(std::string("crypto: libcrypto is missing symbol: ") + name);
            return p;
        };
        sha256 = reinterpret_cast<EVP_sha256_fn>(sym("EVP_sha256"))();
        aes_256_gcm = reinterpret_cast<EVP_aes_256_gcm_fn>(sym("EVP_aes_256_gcm"))();
        ctx_new = reinterpret_cast<EVP_CIPHER_CTX_new_fn>(sym("EVP_CIPHER_CTX_new"));
        ctx_free = reinterpret_cast<EVP_CIPHER_CTX_free_fn>(sym("EVP_CIPHER_CTX_free"));
        ctx_ctrl = reinterpret_cast<EVP_CIPHER_CTX_ctrl_fn>(sym("EVP_CIPHER_CTX_ctrl"));
        encrypt_init = reinterpret_cast<EVP_EncryptInit_ex_fn>(sym("EVP_EncryptInit_ex"));
        encrypt_update = reinterpret_cast<EVP_EncryptUpdate_fn>(sym("EVP_EncryptUpdate"));
        encrypt_final = reinterpret_cast<EVP_EncryptFinal_ex_fn>(sym("EVP_EncryptFinal_ex"));
        decrypt_init = reinterpret_cast<EVP_DecryptInit_ex_fn>(sym("EVP_DecryptInit_ex"));
        decrypt_update = reinterpret_cast<EVP_DecryptUpdate_fn>(sym("EVP_DecryptUpdate"));
        decrypt_final = reinterpret_cast<EVP_DecryptFinal_ex_fn>(sym("EVP_DecryptFinal_ex"));
        pbkdf2 = reinterpret_cast<PKCS5_PBKDF2_HMAC_fn>(sym("PKCS5_PBKDF2_HMAC"));
        rand_bytes = reinterpret_cast<RAND_bytes_fn>(sym("RAND_bytes"));
    }
    ~LibCrypto() {
        if (handle) dlclose(handle);
    }
};

LibCrypto& lib() {
    static LibCrypto instance;
    return instance;
}

// RAII wrapper so an exception thrown mid-encrypt/decrypt (any of the checks below) can't leak
// ctx -- same discipline as this project's other resource-owning types (OpticalLink, etc.).
struct CtxGuard {
    void* ctx;
    LibCrypto* l;
    ~CtxGuard() { if (ctx) l->ctx_free(ctx); }
};

} // namespace

void random_bytes(uint8_t* buf, size_t len) {
    if (lib().rand_bytes(buf, static_cast<int>(len)) != 1)
        throw std::runtime_error("crypto: RAND_bytes failed");
}

std::vector<uint8_t> pbkdf2_hmac_sha256(const std::string& passphrase, const std::vector<uint8_t>& salt,
                                         int iterations, size_t key_len) {
    auto& l = lib();
    std::vector<uint8_t> out(key_len);
    int rc = l.pbkdf2(passphrase.data(), static_cast<int>(passphrase.size()),
                       salt.data(), static_cast<int>(salt.size()),
                       iterations, l.sha256, static_cast<int>(key_len), out.data());
    if (rc != 1) throw std::runtime_error("crypto: PKCS5_PBKDF2_HMAC failed");
    return out;
}

std::vector<uint8_t> aes256gcm_encrypt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv,
                                       const std::vector<uint8_t>& plaintext) {
    if (key.size() != kKeyLen) throw std::runtime_error("crypto: AES-256-GCM key must be 32 bytes");
    if (iv.size() != kIvLen) throw std::runtime_error("crypto: AES-GCM iv must be 12 bytes");
    auto& l = lib();
    void* ctx = l.ctx_new();
    if (!ctx) throw std::runtime_error("crypto: EVP_CIPHER_CTX_new failed");
    CtxGuard guard{ctx, &l};

    if (l.encrypt_init(ctx, l.aes_256_gcm, nullptr, nullptr, nullptr) != 1)
        throw std::runtime_error("crypto: EVP_EncryptInit_ex (cipher select) failed");
    if (l.ctx_ctrl(ctx, kEvpCtrlGcmSetIvLen, static_cast<int>(iv.size()), nullptr) != 1)
        throw std::runtime_error("crypto: setting GCM IV length failed");
    if (l.encrypt_init(ctx, nullptr, nullptr, key.data(), iv.data()) != 1)
        throw std::runtime_error("crypto: EVP_EncryptInit_ex (key/iv) failed");

    std::vector<uint8_t> ciphertext(plaintext.size() + kTagLen); // headroom keeps .data() non-null even if empty
    int len = 0, total = 0;
    if (l.encrypt_update(ctx, ciphertext.data(), &len, plaintext.data(), static_cast<int>(plaintext.size())) != 1)
        throw std::runtime_error("crypto: EVP_EncryptUpdate failed");
    total += len;
    int final_len = 0;
    if (l.encrypt_final(ctx, ciphertext.data() + total, &final_len) != 1)
        throw std::runtime_error("crypto: EVP_EncryptFinal_ex failed");
    total += final_len; // always 0 for GCM (a stream mode, no block padding) but checked, not assumed
    ciphertext.resize(total);

    unsigned char tag[kTagLen];
    if (l.ctx_ctrl(ctx, kEvpCtrlGcmGetTag, static_cast<int>(kTagLen), tag) != 1)
        throw std::runtime_error("crypto: retrieving GCM tag failed");

    std::vector<uint8_t> out;
    out.reserve(ciphertext.size() + kTagLen);
    out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    out.insert(out.end(), tag, tag + kTagLen);
    return out;
}

std::vector<uint8_t> aes256gcm_decrypt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv,
                                       const std::vector<uint8_t>& ciphertext_and_tag) {
    if (key.size() != kKeyLen) throw std::runtime_error("crypto: AES-256-GCM key must be 32 bytes");
    if (iv.size() != kIvLen) throw std::runtime_error("crypto: AES-GCM iv must be 12 bytes");
    if (ciphertext_and_tag.size() < kTagLen) throw std::runtime_error("crypto: ciphertext too short to contain a tag");
    const size_t ct_len = ciphertext_and_tag.size() - kTagLen;

    auto& l = lib();
    void* ctx = l.ctx_new();
    if (!ctx) throw std::runtime_error("crypto: EVP_CIPHER_CTX_new failed");
    CtxGuard guard{ctx, &l};

    if (l.decrypt_init(ctx, l.aes_256_gcm, nullptr, nullptr, nullptr) != 1)
        throw std::runtime_error("crypto: EVP_DecryptInit_ex (cipher select) failed");
    if (l.ctx_ctrl(ctx, kEvpCtrlGcmSetIvLen, static_cast<int>(iv.size()), nullptr) != 1)
        throw std::runtime_error("crypto: setting GCM IV length failed");
    if (l.decrypt_init(ctx, nullptr, nullptr, key.data(), iv.data()) != 1)
        throw std::runtime_error("crypto: EVP_DecryptInit_ex (key/iv) failed");

    std::vector<uint8_t> plaintext(ct_len + kTagLen); // headroom, same nullptr-safety reasoning as encrypt
    int len = 0, total = 0;
    if (l.decrypt_update(ctx, plaintext.data(), &len, ciphertext_and_tag.data(), static_cast<int>(ct_len)) != 1)
        throw std::runtime_error("crypto: EVP_DecryptUpdate failed");
    total += len;

    // Must set the expected tag before the final call -- OpenSSL verifies it there.
    if (l.ctx_ctrl(ctx, kEvpCtrlGcmSetTag, static_cast<int>(kTagLen),
                   const_cast<uint8_t*>(ciphertext_and_tag.data() + ct_len)) != 1)
        throw std::runtime_error("crypto: setting GCM tag failed");

    int final_len = 0;
    // Returns <=0 specifically when the authentication tag fails to verify -- the "wrong
    // passphrase / tampered or foreign data" signal, treated as opaque failure exactly like Web
    // Crypto's own AES-GCM throws instead of returning corrupted plaintext.
    if (l.decrypt_final(ctx, plaintext.data() + total, &final_len) <= 0)
        throw std::runtime_error("crypto: authentication failed (wrong passphrase, or not a valid encrypted message)");
    total += final_len;
    plaintext.resize(total);
    return plaintext;
}

} // namespace qrtp

#endif // _WIN32

// ============================================================================================
// Platform-independent: the message-level wire format, identical to web/messenger.js's
// encryptMessage()/decryptMessage() -- base64(salt(16) || iv(12) || ciphertext+tag).
namespace qrtp {

std::string encrypt_message(const std::string& passphrase, const std::string& plaintext) {
    std::vector<uint8_t> salt(kSaltLen), iv(kIvLen);
    random_bytes(salt.data(), salt.size());
    random_bytes(iv.data(), iv.size());

    auto key = pbkdf2_hmac_sha256(passphrase, salt, kPbkdf2Iterations, kKeyLen);
    std::vector<uint8_t> pt(plaintext.begin(), plaintext.end());
    auto ct = aes256gcm_encrypt(key, iv, pt);

    std::vector<uint8_t> combined;
    combined.reserve(salt.size() + iv.size() + ct.size());
    combined.insert(combined.end(), salt.begin(), salt.end());
    combined.insert(combined.end(), iv.begin(), iv.end());
    combined.insert(combined.end(), ct.begin(), ct.end());
    return base64_encode(combined);
}

std::string decrypt_message(const std::string& passphrase, const std::string& base64_ciphertext) {
    std::vector<uint8_t> combined = base64_decode(base64_ciphertext);
    if (combined.size() < kSaltLen + kIvLen + kTagLen)
        throw std::runtime_error("crypto: encrypted message too short");

    std::vector<uint8_t> salt(combined.begin(), combined.begin() + kSaltLen);
    std::vector<uint8_t> iv(combined.begin() + kSaltLen, combined.begin() + kSaltLen + kIvLen);
    std::vector<uint8_t> ct(combined.begin() + kSaltLen + kIvLen, combined.end());

    auto key = pbkdf2_hmac_sha256(passphrase, salt, kPbkdf2Iterations, kKeyLen);
    auto pt = aes256gcm_decrypt(key, iv, ct);
    return std::string(pt.begin(), pt.end());
}

} // namespace qrtp
