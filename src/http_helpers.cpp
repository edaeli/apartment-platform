#include "http_helpers.h"
#include <charconv>
#include <stdexcept>

drogon::HttpResponsePtr jsonResponse(const Json::Value& value,
                                    drogon::HttpStatusCode status) {
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

std::int64_t parseInteger(const std::string& value, const std::string& name,
                          std::int64_t minimum, std::int64_t maximum) {
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || result < minimum || result > maximum) {
        throw std::invalid_argument("Параметр " + name + " должен быть целым числом от " +
            std::to_string(minimum) + " до " + std::to_string(maximum));
    }
    return result;
}

Json::Value listingToJson(const Listing& item) {
    Json::Value json;
    json["id"] = Json::Int64(item.id);
    json["property_id"] = Json::Int64(item.propertyId);
    json["price"] = Json::Int64(item.price);
    json["seller_user_id"] = item.sellerUserId ? Json::Value(Json::Int64(*item.sellerUserId)) : Json::Value(Json::nullValue);
    json["source_booking_id"] = item.sourceBookingId ? Json::Value(Json::Int64(*item.sourceBookingId)) : Json::Value(Json::nullValue);
    json["created_at"] = item.createdAt;
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