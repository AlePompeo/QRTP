#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qrtp {

// AES-256-GCM + PBKDF2-HMAC-SHA256 encrypted messaging, matching web/messenger.js's wire format
// exactly (salt(16) || iv(12) || ciphertext+tag, base64) so a message encrypted on one platform
// (native GUI or browser) can be decrypted on any other. Linux/macOS: OpenSSL libcrypto, loaded at
// runtime with dlopen (same "no -devel needed at runtime" convention as qr_encoder.cpp). Windows:
// the built-in CNG/BCrypt API (bcrypt.dll, part of Windows itself since Vista — no extra install).

// PBKDF2-HMAC-SHA256, `iterations` rounds, producing `key_len` bytes.
std::vector<uint8_t> pbkdf2_hmac_sha256(const std::string& passphrase, const std::vector<uint8_t>& salt,
                                         int iterations, size_t key_len);

// `key` must be 32 bytes (AES-256), `iv` must be 12 bytes (the standard GCM nonce size). Returns
// ciphertext with the 16-byte authentication tag appended — the same layout Web Crypto's
// crypto.subtle.encrypt({name:'AES-GCM'}, ...) returns, and what decrypt below expects back.
std::vector<uint8_t> aes256gcm_encrypt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv,
                                       const std::vector<uint8_t>& plaintext);

// Throws std::runtime_error if the authentication tag doesn't verify (wrong key, or the data was
// tampered with / isn't really AES-GCM ciphertext at all).
std::vector<uint8_t> aes256gcm_decrypt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv,
                                       const std::vector<uint8_t>& ciphertext_and_tag);

// High-level message API. 100,000 PBKDF2 iterations, random 16-byte salt and 12-byte IV per call
// (from a CSPRNG), wire format base64(salt || iv || ciphertext+tag) — identical to
// web/messenger.js's encryptMessage()/decryptMessage(), so QR codes are interchangeable between
// the web app and any native build using the same shared passphrase.
std::string encrypt_message(const std::string& passphrase, const std::string& plaintext);

// Throws std::runtime_error on malformed input or authentication failure (wrong passphrase, or a
// QR from something else entirely) — callers should catch this and just keep scanning, exactly
// like web/messenger.js's handleScanned() does, never surface partial/garbage plaintext.
std::string decrypt_message(const std::string& passphrase, const std::string& base64_ciphertext);

} // namespace qrtp
