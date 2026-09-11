#include "database.h"

#include <drogon/drogon.h>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
struct Options {
    fs::path root = fs::current_path();
    fs::path db;
    int port = 8080;
    bool initOnly = false;
};

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--init-db") {
            options.initOnly = true;
            continue;
        }
        if (argument != "--root" && argument != "--db" && argument != "--port") {
            throw std::runtime_error("Unknown option: " + argument + ". Use --help.");
        }
        if (++i == argc) throw std::runtime_error("Missing value for " + argument);
        const std::string value = argv[i];
        if (argument == "--root") options.root = value;
        if (argument == "--db") options.db = value;
        if (argument == "--port") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.port);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
                || options.port < 1 || options.port > 65535) {
                throw std::runtime_error("Port must be an integer from 1 to 65535");
            }
        }
    }
    options.root = fs::absolute(options.root).lexically_normal();
    if (options.db.empty()) options.db = options.root / "data/apartments.sqlite3";
    options.db = fs::absolute(options.db).lexically_normal();
    return options;
}

drogon::HttpResponsePtr jsonResponse(const Json::Value& value,
                                    drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(value);
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    return response;
}

drogon::HttpResponsePtr errorResponse(const std::string& message, drogon::HttpStatusCode status) {
    Json::Value value;
    value["error"] = message;
    return jsonResponse(value, status);
}

Json::Value listingToJson(const Listing& item) {
    Json::Value json;
    json["id"] = Json::Int64(item.id);
    json["property_id"] = Json::Int64(item.propertyId);
    json["price"] = Json::Int64(item.price);
    json["currency"] = "AMD";
    json["deal_type"] = item.dealType;
    json["status"] = item.status;
    json["kind"] = item.kind;
    json["address"] = item.address;
    json["district"] = item.district;
    json["description"] = item.description;
    json["area"] = item.area;
    json["rooms"] = item.rooms;
    json["floor"] = item.floor;
    json["latitude"] = item.latitude;
    json["longitude"] = item.longitude;
    json["photos"] = Json::Value(Json::arrayValue);
    for (const auto& photo : item.photos) {
        Json::Value image;
        image["url"] = photo.url;
        image["caption"] = photo.caption;
        json["photos"].append(image);
    }
    return json;
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: apartment_server [--root PATH] [--db PATH] [--port 8080] [--init-db]\n"
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
            Database db(options.db.string(), true);
            db.migrate(schema.string());
            seedDemoData(db);
            std::cout << "Database: " << options.db.string() << '\n'
                      << "Properties: " << db.scalar("SELECT COUNT(*) FROM properties")
                      << ", listings: " << db.scalar("SELECT COUNT(*) FROM listings") << std::endl;
        }
        if (options.initOnly) return 0;

        const std::string dbPath = options.db.string();
        auto& app = drogon::app();
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
            [dbPath](const drogon::HttpRequestPtr& request,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                const auto type = request->getParameter("type");
                if (!type.empty() && type != "rent" && type != "sale") {
                    callback(errorResponse("Параметр type должен быть rent или sale", drogon::k400BadRequest));
                    return;
                }
                try {
                    Database db(dbPath);
                    const auto listings = db.listings(type);
                    Json::Value result;
                    result["items"] = Json::Value(Json::arrayValue);
                    for (const auto& item : listings) result["items"].append(listingToJson(item));
                    result["count"] = Json::UInt64(listings.size());
                    callback(jsonResponse(result));
                } catch (const std::exception& error) {
                    LOG_ERROR << error.what();
                    callback(errorResponse("Не удалось загрузить объявления", drogon::k500InternalServerError));
                }
            }, {drogon::Get});

        std::cout << "Open http://127.0.0.1:" << options.port << " (Ctrl+C to stop)" << std::endl;
        app.setLogLevel(trantor::Logger::kWarn)
           .setDocumentRoot(publicPath.string())
           .setHomePage("index.html")
           .setUploadPath((options.db.parent_path() / "uploads").string())
           .setFileTypes({"html", "css", "js", "jpg", "svg", "ico"})
           .setThreadNum(2)
           .addListener("127.0.0.1", static_cast<std::uint16_t>(options.port))
           .run();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
