#pragma once
#include "database.h"
#include <drogon/drogon.h>

drogon::HttpResponsePtr jsonResponse(const Json::Value& value,
    drogon::HttpStatusCode status = drogon::k200OK);
drogon::HttpResponsePtr errorResponse(const std::string& message, drogon::HttpStatusCode status);
std::int64_t parseInteger(const std::string& value, const std::string& name,
    std::int64_t minimum, std::int64_t maximum);
Json::Value listingToJson(const Listing& item);
