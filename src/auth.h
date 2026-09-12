#pragma once
#include "security.h"
#include <drogon/drogon.h>
#include <set>

// Обработчики HTTP для пользователей, сессий и бронирований.
// SQL находится в accounts.cpp, криптография и память сессий — в security.cpp.
class AuthService {
public:
    AuthService(std::string dbPath, std::string publicPath, int port,
                bool secureCookies, const std::string& origin);
    void registerRoutes(drogon::HttpAppFramework& app);
private:
    using Response = drogon::HttpResponsePtr;
    using Request = drogon::HttpRequestPtr;
    using Action = std::function<Response(const Request&)>;
    Response handle(const Request& request, const Action& action);
    Session requireSession(const Request& request, bool authenticated);
    void checkCsrf(const Request& request, const Session& session);
    void setCookie(const Response& response, const std::string& id, bool expired = false);
    Response authResult(const Session& session, drogon::HttpStatusCode status = drogon::k200OK);
    Response authenticate(const Request& request, bool registration);
    SessionStore sessions_;
    RateLimiter limiter_;
    std::string dbPath_, publicPath_, dummyHash_;
    bool secureCookies_;
    std::set<std::string> allowedOrigins_;
};
