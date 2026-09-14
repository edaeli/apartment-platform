#include "price_estimator.h"
#include <drogon/drogon.h>
#include <cmath>
#include <filesystem>
PriceEstimator::PriceEstimator(const std::string& directory) {
#ifdef BUILD_PRICE_ML
    if(directory.empty()) { LOG_INFO<<"Price ML unavailable: --models-dir not configured";return; }
    for(const std::string deal:{"rent","sale"}) {
        try {
            auto model=price_ml::Model::load((std::filesystem::path(directory)/(deal+".model")).string());
            if(model.deal!=deal) throw std::runtime_error("Model deal does not match filename");
            (deal=="rent"?rent_:sale_)=std::move(model);
            LOG_INFO<<"Price ML loaded: "<<deal;
        } catch(const std::exception& e) { LOG_WARN<<"Price ML unavailable for "<<deal<<": "<<e.what(); }
    }
#else
    (void)directory;
    LOG_INFO<<"Price ML unavailable: server built with BUILD_PRICE_ML=OFF";
#endif
}
Json::Value PriceEstimator::estimate(const Listing& item) const {
    Json::Value result;result["status"]="unavailable";
#ifdef BUILD_PRICE_ML
    const auto& model=item.dealType=="rent"?rent_:sale_;
    if(!model) return result;
    try {
        // Цена объявления, ID и описание не передаются модели как признаки.
        price_ml::Row row{0,{item.area,static_cast<double>(item.rooms),static_cast<double>(item.floor)},
            {item.kind,item.district,item.renovation},0,item.dealType};
        const double amount=model->predict(row);
        if(!std::isfinite(amount) || amount<1 || amount>9007199254740991.0)
            throw std::runtime_error("Prediction outside finite/safe JSON price range");
        result["status"]="available";result["amount_amd"]=amount;
    } catch(const std::exception& e) { LOG_WARN<<"Price ML prediction unavailable: "<<e.what(); }
#else
    (void)item;
#endif
    return result;
}
