#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

void initializeCrypto();
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& hash, const std::string& password);
bool equalToken(const std::string& left, const std::string& right);

struct Session {
    std::string id, csrf;
    std::int64_t userId = 0; // 0 — гостевая сессия для формы входа и CSRF.
    std::chrono::steady_clock::time_point expires;
};

class SessionStore {
public:
    static constexpr int lifetimeSeconds = 12 * 60 * 60;
    explicit SessionStore(std::chrono::seconds lifetime = std::chrono::seconds(lifetimeSeconds))
        : lifetime_(lifetime) {}
    std::optional<Session> find(const std::string& id);
    Session replace(const std::string& oldId, std::int64_t userId);
    void erase(const std::string& id);
private:
    std::mutex mutex_;
    std::map<std::string, Session> sessions_;
    std::chrono::seconds lifetime_;
};

// Короткие окна попыток; mutex защищает только память, бронирование защищает SQLite.
class RateLimiter {
public:
    bool allow(const std::string& key, unsigned limit);
private:
    struct Window { unsigned attempts; std::chrono::steady_clock::time_point until; };
    std::mutex mutex_;
    std::map<std::string, Window> windows_;
};
