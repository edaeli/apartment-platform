#pragma once
#include "database.h"
#include <json/json.h>
#ifdef BUILD_PRICE_ML
#include "price_model.h"
#include <optional>
#endif
// Неизменяемые после запуска модели; безопасны для параллельных чтений.
class PriceEstimator {
public:
    explicit PriceEstimator(const std::string& directory);
    Json::Value estimate(const Listing& item) const;
private:
#ifdef BUILD_PRICE_ML
    std::optional<price_ml::Model> rent_, sale_;
#endif
};
