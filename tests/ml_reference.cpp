// Независимый от HTTP-адаптера вызов сохранённых моделей для интеграционного теста.
#include "price_model.h"
#include <filesystem>
#include <iomanip>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    try {
        auto rent=price_ml::Model::load((std::filesystem::path(argv[2])/"rent.model").string());
        auto sale=price_ml::Model::load((std::filesystem::path(argv[2])/"sale.model").string());
        std::cout<<std::setprecision(17);
        for(const auto& row:price_ml::readRows(argv[1]))
            std::cout<<row.propertyId<<' '<<(row.deal=="rent"?rent:sale).predict(row)<<'\n';
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
