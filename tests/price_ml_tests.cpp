#include "price_model.h"
#include "database.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unistd.h>
namespace fs=std::filesystem;
using namespace price_ml;
void check(bool yes,const std::string& message) { if(!yes) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,"Expected rejection"); }
int main(int argc,char** argv) {
    char pattern[]="/tmp/apartment-ml-test-XXXXXX";
    const char* created=mkdtemp(pattern);
    if(!created) return 1;
    const fs::path temp=created;
    try {
        check(argc==2,"Need schema");
        std::vector<Row> known;
        for(int i=1;i<=20;++i) known.push_back({i,{double(i+30),2,1},{"apartment","Known","good"},1000+100.0*(i+30),"rent"});
        const auto model=train(known,"rent");
        // Независимый аналитический ответ для одной переменной с L2=1 и sum loss.
        const double expected=model.targetMean+20.0/21*100*(known.front().numeric[0]-model.mean[0]);
        check(std::abs(model.predict(known.front())-expected)<1e-7,"QR/ridge analytical solution mismatch");
        check(std::abs(model.baseline-5050)<1e-9,"Even median incorrect");
        auto x=model.encode(known.front());
        check(x.size()==10 && x[0]==1 && std::abs(x[1]-(31-model.mean[0])/model.scale[0])<1e-12,"Numeric/order encoding");
        check(x[4]==1 && x[5]==0 && x[6]==1 && x[7]==0 && x[8]==1 && x[9]==0,"One-hot category encoding");
        auto unseen=known.front();unseen.category[1]="Unseen test-only district";
        auto unknown=model.encode(unseen);check(unknown[6]==0 && unknown[7]==1,"Unknown category bucket");
        auto changed=known.front();changed.propertyId=999;changed.price=1e12;
        check(model.encode(changed)==x && model.predict(changed)==model.predict(known.front()),"Price/id leaked into features");
        check(std::isfinite(model.predict(unseen)),"Unknown prediction not finite");
        auto constant=known;for(auto& r:constant){r.numeric={50,2,1};r.price=1000;}
        auto constantModel=train(constant,"rent");check(std::abs(constantModel.predict(constant[0])-1000)<1e-9,"Constant/collinear data");
        const auto file=(temp/"model.txt").string();model.save(file);const auto restored=Model::load(file);
        for(const auto& r:known)check(restored.predict(r)==model.predict(r),"Serialization not exact");
        check(restored.categories==model.categories && restored.featureNames()==model.featureNames(),"Metadata not preserved");
        {std::ofstream bad(temp/"bad");bad<<"APARTMENT_RIDGE 999\n";}rejects([&]{Model::load((temp/"bad").string());});
        {std::ofstream bad(temp/"bad");bad<<"APARTMENT_RIDGE 1\n";}rejects([&]{Model::load((temp/"bad").string());});
        rejects([&]{auto r=known[0];r.deal="sale";model.predict(r);});
        std::cout<<"PASS: analytical ridge/QR, one-hot/unknown, no id/price features, constant data, finite predictions, exact serialization and invalid models\n";
        auto clippedModel=model;
        std::fill(clippedModel.coefficients.begin(),clippedModel.coefficients.end(),0);
        clippedModel.targetScale=1;
        for(double raw:{-100.0,0.0,0.5,1.0,2.0}) {
            clippedModel.targetMean=raw;
            const auto p=clippedModel.predictDetailed(known[0]);
            check(p.rawAmount==raw && p.lowerClipped==(raw<1),"Clipping flag/boundary mismatch");
            check(p.amount==std::max(1.0,raw) && clippedModel.predict(known[0])==p.amount,"Legacy prediction changed");
            clippedModel.save(file);const auto loaded=Model::load(file);
            check(loaded.predictDetailed(known[0]).lowerClipped==p.lowerClipped,"Clipping flag after load");
        }
        const auto dbpath=(temp/"test.sqlite3").string();
        {
            Database db(dbpath, true); db.migrate(argv[1]);seedDemoData(db);
            db.execute("INSERT INTO users(id,name,email,password_hash) VALUES(1,'Test','test@example.test','test-only')");
            db.execute("UPDATE listings SET status='closed' WHERE id=1");
            db.execute("INSERT INTO bookings(id,user_id,listing_id,property_id,price_at_booking,deal_type,status,ended_at) SELECT 1,1,id,property_id,price,deal_type,'resold','2026-09-14T00:00:00Z' FROM listings WHERE id=1");
            db.execute("INSERT INTO listings(property_id,deal_type,price,seller_user_id,source_booking_id) SELECT property_id,'sale',999999999,1,1 FROM listings WHERE id=1");
        }
        auto bytes=[&]{std::ifstream in(dbpath,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
        const auto before=bytes();const auto rows=readRows(dbpath);check(before==bytes(),"Reader changed database bytes");
        check(rows.size()==1000,"Original selection excluded history or included resale");
        check(std::count_if(rows.begin(),rows.end(),[](const Row& r){return r.propertyId==1;})==1,"Original/closed property must occur once");
        for(const std::string deal:{"rent","sale"}) {
            auto split=splitRows(rows,deal);check(split.train.size()>350 && split.test.size()>50,"Unexpected split sizes");
            std::set<std::int64_t> ids;for(const auto& r:split.train)ids.insert(r.propertyId);
            for(const auto& r:split.test)check(!ids.count(r.propertyId),"Train/test overlap");
            auto reversed=rows;std::reverse(reversed.begin(),reversed.end());auto repeat=splitRows(reversed,deal);
            check(repeat.train.size()==split.train.size() && repeat.test.size()==split.test.size(),"Unstable split");
            for(std::size_t i=0;i<split.train.size();++i)check(repeat.train[i].propertyId==split.train[i].propertyId,"Order-dependent split");
            for(std::size_t i=0;i<split.test.size();++i)check(repeat.test[i].propertyId==split.test[i].propertyId,"Order-dependent test split");
            const auto fitted=train(split.train,deal);const auto again=train(repeat.train,deal);
            check(fitted.coefficients==again.coefficients,"Non-reproducible training");
            // Тестовые признаки/цены не участвуют в fit: экстремальные изменения не меняют модель.
            for(auto& r:split.test){r.numeric[0]=1e6;r.category[1]="TEST_ONLY";r.price=1;}
            const auto fittedAgain=train(split.train,deal);
            check(fittedAgain.mean==fitted.mean && fittedAgain.categories==fitted.categories && fittedAgain.coefficients==fitted.coefficients,"Test leakage");
            for(const auto& row:rows)if(row.deal==deal)check(std::isfinite(fitted.predict(row)),"Non-finite demo prediction");
            const auto path=(temp/(deal+".model")).string();fitted.save(path);const auto loaded=Model::load(path);
            for(const auto& row:rows)if(row.deal==deal)check(fitted.predict(row)==loaded.predict(row),"Demo round-trip mismatch");
        }
        auto duplicates=rows;duplicates.push_back(rows[0]);rejects([&]{splitRows(duplicates,rows[0].deal);});
        auto otherDeal=rows;for(auto& r:otherDeal)r.deal="rent";
        auto allRent=splitRows(otherDeal,"rent");for(auto& r:otherDeal)r.deal="sale";
        auto allSale=splitRows(otherDeal,"sale");check(allRent.test.size()==allSale.test.size(),"Grouping depends on deal");
        for(std::size_t i=0;i<allRent.test.size();++i)check(allRent.test[i].propertyId==allSale.test[i].propertyId,"Property split differs across deals");
        check(before==bytes(),"Tests changed source DB");
        std::cout<<"PASS: isolated schema6 DB, no writes, excludes resale/includes closed original, property split/no overlap, train-only transforms, reproducibility, both models round-trip\n";
        fs::remove_all(temp);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<" (fixture retained at "<<temp<<")\n";return 1;}
}
