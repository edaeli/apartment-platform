#include "auth.h"
#include "http_helpers.h"
#include <algorithm>
#include <limits>
#include <regex>
#include <map>
#include <sstream>

namespace {
const std::string cookieName = "apartment_session";
class HttpError : public std::runtime_error {
public:
    HttpError(drogon::HttpStatusCode status, const std::string& text)
        : std::runtime_error(text), status(status) {}
    drogon::HttpStatusCode status;
};

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

Json::Value body(const drogon::HttpRequestPtr& request, const std::set<std::string>& fields, bool stringsOnly = true) {
    const auto contentType = request->getHeader("content-type");
    const auto json = request->getJsonObject();
    if (contentType.substr(0, contentType.find(';')) != "application/json" || !json || !json->isObject()) {
        throw std::invalid_argument("Ожидается JSON-объект с Content-Type: application/json");
    }
    if (json->size() != fields.size()) throw std::invalid_argument("Неверный набор полей запроса");
    for (const auto& name : json->getMemberNames()) {
        if (!fields.count(name) || (stringsOnly && !(*json)[name].isString())) {
            throw std::invalid_argument("Неверные поля запроса: ожидаются строки");
        }
    }
    return *json;
}

std::int64_t resalePrice(const drogon::HttpRequestPtr& request) {
    const auto value = body(request, {"price"}, false)["price"];
    if (value.isString()) {
        const auto text = value.asString();
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
            throw std::invalid_argument("Цена должна состоять из цифр без пробелов, дробной части и знака");
        }
        return parseInteger(text, "price", 1, Database::maxResalePrice);
    }
    // JsonCpp отличает целую запись от realValue: 1.5, 1.0 и 1e3 не принимаются.
    if ((value.type() != Json::intValue && value.type() != Json::uintValue) ||
        !value.isInt64() || value.asInt64() < 1 || value.asInt64() > Database::maxResalePrice) {
        throw std::invalid_argument("Цена должна быть целым числом от 1 до 1000000000000 драмов");
    }
    return value.asInt64();
}

// Для этих коротких API разрешены только целые ASCII-значения page/page_size/limit.
std::map<std::string, std::int64_t> integerParameters(const drogon::HttpRequestPtr& request,
                                                    const std::map<std::string, std::int64_t>& maximum) {
    std::map<std::string, std::int64_t> result;
    std::istringstream parts(request->query());
    std::string part;
    while (std::getline(parts, part, '&')) {
        const auto equal = part.find('=');
        const auto name = part.substr(0, equal);
        if (equal == std::string::npos || !maximum.count(name) || result.count(name))
            throw std::invalid_argument("Неизвестный или повторяющийся параметр URL");
        const auto value = part.substr(equal + 1);
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("Параметр " + name + " должен состоять из цифр");
        result[name] = parseInteger(value, name, 1, maximum.at(name));
    }
    if (!request->query().empty() && request->query().back() == '&')
        throw std::invalid_argument("Пустой параметр URL");
    return result;
}

Json::Value userJson(const User& user) {
    Json::Value result;
    result["id"] = Json::Int64(user.id);
    result["name"] = user.name;
    result["email"] = user.email;
    result["created_at"] = user.createdAt;
    return result; // password_hash намеренно не сериализуется.
}
}

AuthService::AuthService(std::string dbPath, std::string publicPath, int port,
                         bool secureCookies, const std::string& origin)
    : dbPath_(std::move(dbPath)), publicPath_(std::move(publicPath)), secureCookies_(secureCookies) {
    initializeCrypto();
    dummyHash_ = hashPassword("dummy value for unknown account verification");
    if (origin.empty()) {
        allowedOrigins_.insert("http://127.0.0.1:" + std::to_string(port));
        allowedOrigins_.insert("http://localhost:" + std::to_string(port));
    } else {
        // Origin — только схема, хост и необязательный порт; без пути и userinfo.
        static const std::regex pattern(R"(^https?://[a-zA-Z0-9.-]+(:[0-9]{1,5})?$)");
        if (!std::regex_match(origin, pattern)) throw std::runtime_error("Invalid --origin");
        if (origin.rfind("https://", 0) == 0 && !secureCookies_) {
            throw std::runtime_error("HTTPS origin requires --secure-cookies");
        }
        allowedOrigins_.insert(origin);
    }
}

AuthService::Response AuthService::handle(const Request& request, const Action& action, bool allowQuery) {
    try {
        if (!allowQuery && !request->query().empty()) throw std::invalid_argument("Этот API не принимает параметры URL");
        return action(request);
    } catch (const HttpError& error) {
        auto response = errorResponse(error.what(), error.status);
        if (error.status == drogon::k429TooManyRequests) response->addHeader("Retry-After", "300");
        return response;
    } catch (const BookingError& error) {
        const auto status = error.reason == BookingFailure::notFound ? drogon::k404NotFound :
            error.reason == BookingFailure::forbidden ? drogon::k403Forbidden :
            error.reason == BookingFailure::conflict ? drogon::k409Conflict : drogon::k503ServiceUnavailable;
        auto response = errorResponse(error.what(), status);
        if (status == drogon::k503ServiceUnavailable) response->addHeader("Retry-After", "2");
        return response;
    } catch (const std::invalid_argument& error) {
        return errorResponse(error.what(), drogon::k400BadRequest);
    } catch (const SqliteError& error) {
        // Не журналируем тело запроса, email, пароль, хеш или идентификатор сессии.
        LOG_ERROR << "Account database operation failed, SQLite code " << error.code;
        return errorResponse(error.busy() ? "База данных временно занята. Повторите позже" :
            "Не удалось сохранить данные. Попробуйте позже",
            error.busy() ? drogon::k503ServiceUnavailable : drogon::k500InternalServerError);
    } catch (const std::exception&) {
        LOG_ERROR << "Account operation failed";
        return errorResponse("Сервис временно недоступен. Попробуйте позже", drogon::k503ServiceUnavailable);
    }
}

std::int64_t AuthService::currentUserId(const Request& request) {
    const auto session = sessions_.find(request->getCookie(cookieName));
    return session ? session->userId : 0;
}

Session AuthService::requireSession(const Request& request, bool authenticated) {
    const auto session = sessions_.find(request->getCookie(cookieName));
    if (authenticated && (!session || session->userId == 0)) {
        throw HttpError(drogon::k401Unauthorized, "Войдите в свой аккаунт");
    }
    if (!session) throw HttpError(drogon::k403Forbidden, "Сессия формы истекла. Обновите страницу");
    return *session;
}

void AuthService::checkCsrf(const Request& request, const Session& session) {
    const auto& origin = request->getHeader("origin");
    if ((!origin.empty() && !allowedOrigins_.count(origin)) ||
        request->getHeader("sec-fetch-site") == "cross-site" ||
        !equalToken(session.csrf, request->getHeader("x-csrf-token"))) {
        throw HttpError(drogon::k403Forbidden, "Проверка безопасности формы не пройдена. Обновите страницу");
    }
}

void AuthService::setCookie(const Response& response, const std::string& id, bool expired) {
    drogon::Cookie cookie(cookieName, id);
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSecure(secureCookies_);
    cookie.setSameSite(drogon::Cookie::SameSite::kLax);
    cookie.setMaxAge(expired ? 0 : SessionStore::lifetimeSeconds);
    response->addCookie(cookie);
}

AuthService::Response AuthService::authResult(const Session& session, drogon::HttpStatusCode status) {
    Json::Value result;
    result["user"] = Json::nullValue;
    if (session.userId) {
        Database db(dbPath_);
        const auto user = db.userById(session.userId);
        if (!user) {
            sessions_.erase(session.id);
            throw HttpError(drogon::k401Unauthorized, "Аккаунт недоступен. Войдите снова");
        }
        result["user"] = userJson(*user);
    }
    result["csrf_token"] = session.csrf;
    return jsonResponse(result, status);
}

AuthService::Response AuthService::authenticate(const Request& request, bool registration) {
    const auto session = requireSession(request, false);
    checkCsrf(request, session);
    const auto data = body(request, registration ? std::set<std::string>{"name", "email", "password"} :
                                                 std::set<std::string>{"email", "password"});
    std::string email = trim(data["email"].asString());
    std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    });
    // Учебная форма поддерживает обычные ASCII email, без quoted/local IDN адресов.
    static const std::regex emailPattern(R"(^[a-z0-9!#$%&'*+/=?^_`{|}~-]+(\.[a-z0-9!#$%&'*+/=?^_`{|}~-]+)*@[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$)");
    const auto password = data["password"].asString();
    if (email.size() > 254 || !std::regex_match(email, emailPattern)) {
        throw std::invalid_argument("Укажите корректный email латиницей, например name@example.com");
    }
    if (password.size() < 8 || password.size() > 128 || password.find('\0') != std::string::npos) {
        throw std::invalid_argument("Пароль должен содержать от 8 до 128 байт (символы кириллицы занимают больше одного байта)");
    }
    const auto ip = request->peerAddr().toIp(); // Не доверяем X-Forwarded-For браузера.
    const bool ipAllowed = limiter_.allow((registration ? "register:" : "login:") + ip, registration ? 10 : 30);
    const bool emailAllowed = registration || limiter_.allow("email:" + email, 5);
    if (!ipAllowed || !emailAllowed) {
        throw HttpError(drogon::k429TooManyRequests, "Слишком много попыток. Повторите через 5 минут");
    }
    Database db(dbPath_);
    std::int64_t userId;
    if (registration) {
        const auto name = trim(data["name"].asString());
        const auto chars = std::count_if(name.begin(), name.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
        if (chars < 2 || chars > 80 || std::any_of(name.begin(), name.end(),
            [](unsigned char c) { return c < 32 || c == 127; })) {
            throw std::invalid_argument("Имя должно содержать от 2 до 80 символов без управляющих знаков");
        }
        const auto hash = hashPassword(password);
        try {
            userId = db.createUser(name, email, hash);
        } catch (const SqliteError& error) {
            if (error.code == SQLITE_CONSTRAINT_UNIQUE) {
                throw HttpError(drogon::k409Conflict, "Этот email уже зарегистрирован");
            }
            throw;
        }
    } else {
        const auto user = db.userByEmail(email);
        const bool valid = verifyPassword(user ? user->passwordHash : dummyHash_, password);
        if (!valid || !user) throw HttpError(drogon::k401Unauthorized, "Неверный email или пароль");
        userId = user->id;
    }
    const auto renewed = sessions_.replace(session.id, userId);
    auto response = authResult(renewed, registration ? drogon::k201Created : drogon::k200OK);
    setCookie(response, renewed.id);
    return response;
}

void AuthService::registerRoutes(drogon::HttpAppFramework& app) {
    // Общая оболочка переводит исключения в понятные HTTP-ответы.
    const auto route = [this, &app](const std::string& path, drogon::HttpMethod method, Action action) {
        app.registerHandler(path, [this, action](const Request& request,
            std::function<void(const Response&)>&& callback) { callback(handle(request, action)); }, {method});
    };
    route("/api/auth/me", drogon::Get, [this](const Request& request) {
        auto session = sessions_.find(request->getCookie(cookieName));
        const bool created = !session;
        if (!session) session = sessions_.replace("", 0);
        auto response = authResult(*session);
        if (created) setCookie(response, session->id);
        return response;
    });
    route("/api/auth/register", drogon::Post, [this](const Request& r) { return authenticate(r, true); });
    route("/api/auth/login", drogon::Post, [this](const Request& r) { return authenticate(r, false); });
    route("/api/auth/logout", drogon::Post, [this](const Request& request) {
        const auto session = requireSession(request, true);
        checkCsrf(request, session);
        body(request, {});
        sessions_.erase(session.id);
        Json::Value result; result["message"] = "Вы вышли из аккаунта";
        auto response = jsonResponse(result);
        setCookie(response, "", true);
        return response;
    });
    route("/api/bookings", drogon::Get, [this](const Request& request) {
        const auto session = requireSession(request, true);
        Database db(dbPath_);
        Json::Value result; result["items"] = Json::Value(Json::arrayValue);
        for (const auto& item : db.bookingsForUser(session.userId)) {
            Json::Value json;
            json["id"] = Json::Int64(item.id);
            json["listing_id"] = Json::Int64(item.listingId);
            json["price_at_booking"] = Json::Int64(item.price);
            json["deal_type"] = item.dealType;
            json["status"] = item.status;
            json["created_at"] = item.createdAt;
            json["ended_at"] = item.endedAt.empty() ? Json::Value(Json::nullValue) : Json::Value(item.endedAt);
            json["listing"] = listingToJson(item.listing);
            json["resale_listing_id"] = item.resaleListingId ? Json::Value(Json::Int64(*item.resaleListingId)) : Json::Value(Json::nullValue);
            result["items"].append(json);
        }
        return jsonResponse(result);
    });
    app.registerHandler("/api/listings/{1}/book", [this](const Request& request,
        std::function<void(const Response&)>&& callback, const std::string& value) {
        callback(handle(request, [this, &value](const Request& r) {
            const auto session = requireSession(r, true);
            checkCsrf(r, session);
            body(r, {}); // user_id и цена в запросе не принимаются.
            const auto id = parseInteger(value, "id", 1, std::numeric_limits<std::int64_t>::max());
            Database db(dbPath_);
            const auto bookingId = db.book(session.userId, id);
            Json::Value result;
            result["id"] = Json::Int64(bookingId);
            result["listing_id"] = Json::Int64(id);
            result["status"] = "active";
            result["message"] = "Бронирование создано. Оплата не производится";
            return jsonResponse(result, drogon::k201Created);
        }));
    }, {drogon::Post});
    for (const bool resale : {false, true}) {
        const std::string path = resale ? "/api/bookings/{1}/resell" : "/api/bookings/{1}/release";
        app.registerHandler(path, [this, resale](const Request& request,
            std::function<void(const Response&)>&& callback, const std::string& value) {
            callback(handle(request, [this, resale, &value](const Request& r) {
                const auto session = requireSession(r, true);
                checkCsrf(r, session);
                const auto id = parseInteger(value, "id", 1, std::numeric_limits<std::int64_t>::max());
                const auto price = resale ? resalePrice(r) : 0;
                if (!resale) body(r, {});
                Database db(dbPath_);
                Json::Value result;
                result["booking_id"] = Json::Int64(id);
                if (resale) {
                    result["listing_id"] = Json::Int64(db.resellBooking(session.userId, id, price));
                    result["status"] = "resold";
                    result["message"] = "Новое объявление опубликовано в учебном каталоге. Оплата не производится";
                } else {
                    db.releaseBooking(session.userId, id);
                    result["status"] = "released";
                    result["message"] = "Бронирование завершено. Жильё снова доступно";
                }
                return jsonResponse(result, resale ? drogon::k201Created : drogon::k200OK);
            }));
        }, {drogon::Post});
    }
    // Отдельные операции установки и удаления, без серверного переключателя состояния.
    for (const auto method : {drogon::Put, drogon::Delete}) {
        app.registerHandler("/api/favorites/{1}", [this, method](const Request& request,
            std::function<void(const Response&)>&& callback, const std::string& value) {
            callback(handle(request, [this, method, &value](const Request& r) {
                const auto session = requireSession(r, true);
                checkCsrf(r, session); body(r, {});
                const auto id = parseInteger(value, "id", 1, std::numeric_limits<std::int64_t>::max());
                Database db(dbPath_);
                db.setFavorite(session.userId, id, method == drogon::Put);
                Json::Value result; result["listing_id"] = Json::Int64(id);
                result["is_favorite"] = method == drogon::Put;
                return jsonResponse(result);
            }));
        }, {method});
    }
    app.registerHandler("/api/listings/{1}/view", [this](const Request& request,
        std::function<void(const Response&)>&& callback, const std::string& value) {
        callback(handle(request, [this, &value](const Request& r) {
            const auto session = requireSession(r, true);
            checkCsrf(r, session); body(r, {});
            const auto id = parseInteger(value, "id", 1, std::numeric_limits<std::int64_t>::max());
            Database db(dbPath_);
            Json::Value result; result["counted"] = db.recordView(session.userId, id);
            return jsonResponse(result);
        }));
    }, {drogon::Post});
    for (const bool views : {false, true}) {
        app.registerHandler(views ? "/api/views" : "/api/favorites", [this, views](const Request& request,
            std::function<void(const Response&)>&& callback) {
            callback(handle(request, [this, views](const Request& r) {
                const auto session = requireSession(r, true);
                const auto params = integerParameters(r, {{"page", 1000000}, {"page_size", 100}});
                const auto page = params.count("page") ? params.at("page") : 1;
                const auto size = params.count("page_size") ? params.at("page_size") : 24;
                Database db(dbPath_);
                const auto data = db.personalPage(session.userId, views, page, size);
                Json::Value result; result["items"] = Json::Value(Json::arrayValue);
                for (const auto& entry : data.items) {
                    auto item = listingToJson(entry.listing);
                    if (views) {
                        item["last_viewed_at"] = Json::Int64(entry.lastViewedAt);
                        item["view_count"] = Json::Int64(entry.viewCount);
                    } else item["saved_at"] = entry.savedAt;
                    result["items"].append(item);
                }
                result["total"] = Json::Int64(data.total);
                result["count"] = Json::UInt64(data.items.size());
                result["page"] = Json::Int64(page); result["page_size"] = Json::Int64(size);
                result["total_pages"] = Json::Int64((data.total + size - 1) / size);
                return jsonResponse(result);
            }, true));
        }, {drogon::Get});
    }
    app.registerHandler("/api/recommendations", [this](const Request& request,
        std::function<void(const Response&)>&& callback) {
        callback(handle(request, [this](const Request& r) {
            const auto params = integerParameters(r, {{"limit", 24}});
            const int limit = params.count("limit") ? static_cast<int>(params.at("limit")) : 6;
            Database db(dbPath_);
            Json::Value result; result["items"] = Json::Value(Json::arrayValue);
            bool personalized = false;
            for (const auto& entry : db.recommendations(currentUserId(r), limit)) {
                auto item = listingToJson(entry.listing);
                item["score"] = entry.score; item["reason"] = entry.reason;
                personalized |= entry.score > 0;
                result["items"].append(item);
            }
            result["personalized"] = personalized;
            result["count"] = result["items"].size();
            return jsonResponse(result);
        }, true));
    }, {drogon::Get});
    for (const auto& path : {"/favorites", "/view-history", "/personal.html"}) {
        app.registerHandler(path, [this, path = std::string(path)](const Request& request,
            std::function<void(const Response&)>&& callback) {
            const auto target = path == "/view-history" ? "/view-history" : "/favorites";
            auto response = currentUserId(request) ? drogon::HttpResponse::newFileResponse(publicPath_ + "/personal.html") :
                drogon::HttpResponse::newRedirectionResponse("/login?next=" + drogon::utils::urlEncode(target));
            response->addHeader("Cache-Control", "no-store"); callback(response);
        }, {drogon::Get});
    }
    for (const auto& path : {"/login", "/register", "/my-bookings", "/bookings.html"}) {
        app.registerHandler(path, [this, path = std::string(path)](const Request& request,
            std::function<void(const Response&)>&& callback) {
            const auto session = sessions_.find(request->getCookie(cookieName));
            Response response;
            const bool bookingsPage = path == "/my-bookings" || path == "/bookings.html";
            if (bookingsPage && (!session || !session->userId)) {
                response = drogon::HttpResponse::newRedirectionResponse("/login?next=%2Fmy-bookings");
            } else {
                response = drogon::HttpResponse::newFileResponse(publicPath_ +
                    (bookingsPage ? "/bookings.html" : "/auth.html"));
            }
            response->addHeader("Cache-Control", "no-store");
            callback(response);
        }, {drogon::Get});
    }
}
