#include "security.h"
#include <sodium.h>
#include <stdexcept>

namespace {
std::string randomToken() {
    unsigned char bytes[32];
    char hex[65];
    randombytes_buf(bytes, sizeof bytes);
    sodium_bin2hex(hex, sizeof hex, bytes, sizeof bytes);
    return hex;
}
}

void initializeCrypto() {
    if (sodium_init() < 0) throw std::runtime_error("Cannot initialize libsodium");
}

std::string hashPassword(const std::string& password) {
    char result[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str_alg(result, password.data(), password.size(),
            crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw std::runtime_error("Cannot allocate memory for password hashing");
    }
    return result; // Строка содержит алгоритм, настройки, случайную соль и хеш.
}

bool verifyPassword(const std::string& hash, const std::string& password) {
    return crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) == 0;
}

bool equalToken(const std::string& left, const std::string& right) {
    return left.size() == 64 && right.size() == 64 &&
        sodium_memcmp(left.data(), right.data(), 64) == 0;
}

std::optional<Session> SessionStore::find(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) return std::nullopt;
    if (found->second.expires <= std::chrono::steady_clock::now()) {
        sessions_.erase(found);
        return std::nullopt;
    }
    return found->second;
}

Session SessionStore::replace(const std::string& oldId, std::int64_t userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.expires <= now) it = sessions_.erase(it); else ++it;
    }
    if (sessions_.size() >= 4096 && !sessions_.count(oldId)) {
        throw std::runtime_error("Session capacity reached");
    }
    Session session{randomToken(), randomToken(), userId, now + lifetime_};
    // Сначала создаём новую запись: если выделение памяти не удалось, старая остаётся.
    sessions_.emplace(session.id, session);
    sessions_.erase(oldId);
    return session;
}

void SessionStore::erase(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
}

bool RateLimiter::allow(const std::string& key, unsigned limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (it->second.until <= now) it = windows_.erase(it); else ++it;
    }
    if (!windows_.count(key) && windows_.size() >= 10000) return false;
    auto [it, inserted] = windows_.try_emplace(key, Window{0, now + std::chrono::minutes(5)});
    if (it->second.attempts >= limit) return false;
    ++it->second.attempts;
    return true;
}
