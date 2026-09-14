#include "price_estimator.h"
#include "database.h"
#include "seed.h"
#include "http_helpers.h"
#include "auth.h"

#include <drogon/drogon.h>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
struct Options {
    fs::path root = fs::current_path();
    fs::path db;
    int port = 8080;
    bool initOnly = false;
    bool previewDemo = false, refreshDemo = false;
    bool secureCookies = false;
    std::string origin;
    std::string modelsDir;
};

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--preview-demo-update") { options.previewDemo = true; continue; }
        if (argument == "--update-demo-data") { options.refreshDemo = true; continue; }
        if (argument == "--secure-cookies") { options.secureCookies = true; continue; }
        if (argument == "--init-db") {
            options.initOnly = true;
            continue;
        }
        if (argument != "--root" && argument != "--db" && argument != "--port" && argument != "--origin" && argument != "--models-dir") {
            throw std::runtime_error("Unknown option: " + argument + ". Use --help.");
        }
        if (++i == argc) throw std::runtime_error("Missing value for " + argument);
        const std::string value = argv[i];
        if (argument == "--root") options.root = value;
        if (argument == "--db") options.db = value;
        if (argument == "--origin") options.origin = value;
        if (argument == "--models-dir") options.modelsDir = fs::absolute(value).lexically_normal().string();
        if (argument == "--port") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.port);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
                || options.port < 1 || options.port > 65535) {
                throw std::runtime_error("Port must be an integer from 1 to 65535");
            }
        }
    }
    if (static_cast<int>(options.initOnly) + options.previewDemo + options.refreshDemo > 1)
        throw std::runtime_error("Choose only one initialization/demo command");
    options.root = fs::absolute(options.root).lexically_normal();
    if (options.db.empty()) options.db = options.root / "data/apartments.sqlite3";
    options.db = fs::absolute(options.db).lexically_normal();
    return options;
}

ListingFilters parseFilters(const drogon::HttpRequestPtr& request) {
    const std::set<std::string> allowed{
        "type", "district", "min_price", "max_price", "rooms", "sort", "page", "page_size"
    };
    // getParameters хранит одно значение на ключ. Повторы отвергаем до его использования.
    std::set<std::string> seen;
    const auto& raw = request->query();
    for (std::size_t begin = 0; begin < raw.size();) {
        const auto end = raw.find('&', begin);
        const auto part = raw.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.find('=') == std::string::npos || part.back() == '=') {
            throw std::invalid_argument("Каждый параметр должен иметь непустое значение");
        }
        const std::string hex = "0123456789abcdefABCDEF";
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (part[i] != '%') continue;
            if (i + 2 >= part.size() || hex.find(part[i + 1]) == std::string::npos ||
                hex.find(part[i + 2]) == std::string::npos) {
                throw std::invalid_argument("Некорректное кодирование параметров URL");
            }
            i += 2;
        }
        const auto key = drogon::utils::urlDecode(part.substr(0, part.find('=')));
        if (!allowed.count(key) || !seen.insert(key).second) {
            throw std::invalid_argument("Неизвестный или повторяющийся параметр запроса");
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    ListingFilters filters;
    for (const auto& [name, value] : request->getParameters()) {
        if (!allowed.count(name) || value.empty()) {
            throw std::invalid_argument("Неизвестный или пустой параметр запроса");
        }
        if (name == "type") {
            if (value != "rent" && value != "sale") {
                throw std::invalid_argument("Параметр type должен быть rent или sale");
            }
            filters.type = value;
        } else if (name == "district") {
            if (value.size() > 100 || std::any_of(value.begin(), value.end(),
                [](unsigned char c) { return c < 32 || c == 127; })) {
                throw std::invalid_argument("Название района должно содержать до 100 байт без управляющих символов");
            }
            filters.district = value;
        } else if (name == "sort") {
            if (value != "price_asc" && value != "price_desc" && value != "area_asc" && value != "area_desc") {
                throw std::invalid_argument("Параметр sort: price_asc, price_desc, area_asc или area_desc");
            }
            filters.sort = value;
        } else if (name == "rooms") {
            filters.rooms = parseInteger(value, name, 1, 100);
        } else if (name == "page") {
            filters.page = parseInteger(value, name, 1, 1000000);
        } else if (name == "page_size") {
            filters.pageSize = parseInteger(value, name, 1, 100);
        } else if (name == "min_price") {
            filters.minPrice = parseInteger(value, name, 0, 1000000000000LL);
        } else if (name == "max_price") {
            filters.maxPrice = parseInteger(value, name, 0, 1000000000000LL);
        }
    }
    if (filters.minPrice && filters.maxPrice && *filters.minPrice > *filters.maxPrice) {
        throw std::invalid_argument("Минимальная цена не должна превышать максимальную");
    }
    return filters;
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: apartment_server [--root PATH] [--db PATH] [--port 8080] [--init-db | --preview-demo-update | --update-demo-data] [--secure-cookies] [--origin https://example.com] [--models-dir PATH]\n"
                      << "Default root: current directory. Default database: ROOT/data/apartments.sqlite3\n";
            return 0;
        }
        const auto options = parseOptions(argc, argv);
        const auto schema = options.root / "sql/001_initial.sql";
        const auto publicPath = options.root / "public";
        if (!fs::is_regular_file(schema) || !fs::is_regular_file(publicPath / "index.html")) {
            throw std::runtime_error("Run from the project directory or pass --root /path/to/project");
        }
        fs::create_directories(options.db.parent_path());
        {
            Database db(options.db.string(), !options.previewDemo && !options.refreshDemo);
            db.migrate(schema.string());
            if (options.previewDemo || options.refreshDemo) {
                const auto result = refreshDemoData(db, options.refreshDemo);
                std::cout << (options.refreshDemo ? "Updated: " : "Eligible: ") << result.eligible
                          << ", protected: " << result.protectedCount << ", changed/ambiguous: " << result.changed
                          << ", already current: " << result.alreadyCurrent << ", missing: " << result.missing << std::endl;
                return 0;
            }
            seedDemoData(db);
            std::cout << "Database: " << options.db.string() << '\n'
                      << "Properties: " << db.scalar("SELECT COUNT(*) FROM properties")
                      << ", listings: " << db.scalar("SELECT COUNT(*) FROM listings") << std::endl;
        }
        if (options.initOnly) return 0;

        const PriceEstimator priceEstimator(options.modelsDir);
        const std::string dbPath = options.db.string();
        auto& app = drogon::app();
        AuthService auth(dbPath, publicPath.string(), options.port, options.secureCookies, options.origin);
        auth.registerRoutes(app);
        app.registerHandler("/api/health",
            [dbPath](const drogon::HttpRequestPtr&,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                try {
                    Database db(dbPath);
                    Json::Value health;
                    health["status"] = "ok";
                    health["database"] = "ok";
                    health["sqlite_version"] = sqlite3_libversion();
                    health["schema_version"] = Json::Int64(db.scalar("PRAGMA user_version"));
                    health["foreign_keys"] = db.scalar("PRAGMA foreign_keys") == 1;
                    health["properties_count"] = Json::Int64(db.scalar("SELECT COUNT(*) FROM properties"));
                    health["listings_count"] = Json::Int64(db.scalar("SELECT COUNT(*) FROM listings"));
                    callback(jsonResponse(health));
                } catch (const std::exception& error) {
                    LOG_ERROR << error.what();
                    callback(errorResponse("База данных недоступна", drogon::k503ServiceUnavailable));
                }
            }, {drogon::Get});

        app.registerHandler("/api/listings",
            [dbPath, &auth](const drogon::HttpRequestPtr& request,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                try {
                    const auto filters = parseFilters(request);
                    Database db(dbPath);
                    auto page = db.listings(filters);
                    db.markFavorites(page.items, auth.currentUserId(request));
                    const auto pages = (page.total + filters.pageSize - 1) / filters.pageSize;
                    Json::Value result;
                    result["items"] = Json::Value(Json::arrayValue);
                    for (const auto& item : page.items) result["items"].append(listingToJson(item));
                    result["count"] = Json::UInt64(page.items.size());
                    result["total"] = Json::Int64(page.total);
                    result["page"] = Json::Int64(filters.page);
                    result["page_size"] = Json::Int64(filters.pageSize);
                    result["total_pages"] = Json::Int64(pages);
                    result["has_previous"] = filters.page > 1 && page.total > 0;
                    result["has_next"] = filters.page < pages;
                    result["districts"] = Json::Value(Json::arrayValue);
                    for (const auto& district : db.districts()) result["districts"].append(district);
                    callback(jsonResponse(result));
                } catch (const std::invalid_argument& error) {
                    callback(errorResponse(error.what(), drogon::k400BadRequest));
                } catch (const std::exception& error) {
                    LOG_ERROR << error.what();
                    callback(errorResponse("Не удалось загрузить объявления", drogon::k500InternalServerError));
                }
            }, {drogon::Get});

        app.registerHandler("/api/listings/{1}",
            [dbPath, &auth, &priceEstimator](const drogon::HttpRequestPtr& request,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& value) {
                try {
                    const auto id = parseInteger(value, "id", 1, std::numeric_limits<std::int64_t>::max());
                    Database db(dbPath);
                    const auto item = db.listing(id);
                    if (!item) {
                        callback(errorResponse("Объявление не найдено", drogon::k404NotFound));
                        return;
                    }
                    std::vector<Listing> items{*item};
                    db.markFavorites(items, auth.currentUserId(request));
                    auto json = listingToJson(items.front());
                    json["price_estimate"] = priceEstimator.estimate(items.front());
                    callback(jsonResponse(json));
                } catch (const std::invalid_argument& error) {
                    callback(errorResponse(error.what(), drogon::k400BadRequest));
                } catch (const std::exception& error) {
                    LOG_ERROR << error.what();
                    callback(errorResponse("Не удалось загрузить объявление", drogon::k500InternalServerError));
                }
            }, {drogon::Get});

        app.registerHandler("/listings/{1}",
            [dbPath, publicPath](const drogon::HttpRequestPtr&,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& value) {
                auto response = drogon::HttpResponse::newFileResponse((publicPath / "listing.html").string());
                response->addHeader("Cache-Control", "no-store");
                try {
                    const auto id = parseInteger(value, "id", 1, std::numeric_limits<std::int64_t>::max());
                    Database db(dbPath);
                    if (!db.listing(id)) response->setStatusCode(drogon::k404NotFound);
                } catch (const std::invalid_argument&) {
                    response->setStatusCode(drogon::k400BadRequest);
                } catch (const std::exception& error) {
                    LOG_ERROR << error.what();
                    response->setStatusCode(drogon::k503ServiceUnavailable);
                }
                callback(response);
            }, {drogon::Get});

        std::cout << "Open http://127.0.0.1:" << options.port << " (Ctrl+C to stop)" << std::endl;
        app.setLogLevel(trantor::Logger::kWarn)
           .setDocumentRoot(publicPath.string())
           .setHomePage("index.html")
           .setUploadPath((options.db.parent_path() / "uploads").string())
           .setFileTypes({"html", "css", "js", "jpg", "svg", "ico"})
           .setClientMaxBodySize(16 * 1024)
           .setThreadNum(2)
           .addListener("127.0.0.1", static_cast<std::uint16_t>(options.port))
           .run();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
