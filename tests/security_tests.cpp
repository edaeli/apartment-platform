#include "security.h"
#include <iostream>
#include <stdexcept>

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    try {
        initializeCrypto();
        const auto one = hashPassword("test password");
        const auto two = hashPassword("test password");
        check(one.rfind("$argon2id$", 0) == 0 && one != two, "Argon2id or random salts missing");
        check(verifyPassword(one, "test password") && !verifyPassword(one, "wrong password"), "Password verification failed");
        SessionStore store;
        const auto guest = store.replace("", 0);
        const auto user = store.replace(guest.id, 42);
        check(!store.find(guest.id) && store.find(user.id)->userId == 42, "Session not rotated");
        check(guest.csrf != user.csrf && user.csrf != user.id && equalToken(user.csrf, user.csrf), "Invalid CSRF token");
        check(!equalToken(user.csrf, guest.csrf) && !equalToken("", ""), "Invalid token accepted");
        store.erase(user.id);
        check(!store.find(user.id), "Logout did not invalidate session");
        SessionStore expired(std::chrono::seconds(0));
        const auto old = expired.replace("", 42);
        check(!expired.find(old.id), "Expired session accepted");
        RateLimiter limiter;
        for (int i = 0; i < 5; ++i) check(limiter.allow("account", 5), "Premature rate limit");
        check(!limiter.allow("account", 5) && limiter.allow("another", 5), "Rate limit not enforced or not scoped");
        std::cout << "PASS: Argon2id, salts, password verification, rotation, expiry, logout and rate limits\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
