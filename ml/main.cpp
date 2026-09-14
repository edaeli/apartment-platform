#include "price_model.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
int main(int argc,char** argv) {
    try {
        if(argc!=5 || std::string(argv[1])!="--db" || std::string(argv[3])!="--out") {
            std::cerr<<"Usage: price_ml --db PATH --out NEW_DIRECTORY\nReads schema 6 without modifying it. Synthetic data only.\n";
            return 2;
        }
        const fs::path out=argv[4];
        if(fs::exists(out)) throw std::runtime_error("Output already exists; use a new directory");
        const auto rows=price_ml::readRows(argv[2]);
        if(rows.empty()) throw std::runtime_error("No original demo offers");
        fs::create_directories(out);
        std::ofstream report(out/"evaluation.txt");
        report<<std::setprecision(17)<<"format_version=1\nseed="<<price_ml::seed
          <<"\nsplit=SplitMix64(property_id+seed)%5; test bucket 0\nlambda=1 (fixed before evaluation)\n"
          <<"selection=canonical demo-001..demo-1000; matching property/listing demo_key; seller/source NULL; all statuses\n"
          <<"data=current stored features/prices of original offers; synthetic, not market appraisal\n";
        std::cout<<std::fixed<<std::setprecision(2);
        for(const std::string deal:{"rent","sale"}) {
            const auto split=price_ml::splitRows(rows,deal);
            const auto model=price_ml::train(split.train,deal);
            const double error=price_ml::mae(model,split.test), baseline=price_ml::mae(model,split.test,true);
            const auto file=(out/(deal+".model")).string(); model.save(file);
            const auto restored=price_ml::Model::load(file);
            for(const auto& row:rows) if(row.deal==deal && model.predict(row)!=restored.predict(row))
                throw std::runtime_error("Round-trip prediction mismatch");
            std::cout<<deal<<": train="<<split.train.size()<<", test="<<split.test.size()
              <<", MAE="<<error<<" AMD, median MAE="<<baseline<<" AMD, improvement=";
            if(baseline>0) std::cout<<100*(baseline-error)/baseline<<"%";
            else std::cout<<"undefined (baseline MAE=0)";
            std::cout<<(error<baseline?" [better than baseline]":" [NO improvement]")<<"; round-trip exact\n";
            report<<deal<<" train="<<split.train.size()<<" test="<<split.test.size()
              <<" mae_amd="<<error<<" baseline_mae_amd="<<baseline<<" baseline_median="<<model.baseline;
            if(baseline>0) report<<" improvement_percent="<<100*(baseline-error)/baseline;
            else report<<" improvement_percent=undefined";
            report<<'\n';
            for(const auto* part:{&split.train,&split.test}) {
                report<<(part==&split.train?"train_property_ids=":"test_property_ids=");
                for(const auto& r:*part) report<<r.propertyId<<',';
                report<<'\n';
            }
        }
        report.close(); if(!report) throw std::runtime_error("Cannot write evaluation report");
        std::cout<<"Artifacts: "<<out<<"\nSynthetic demonstration, NOT a market valuation.\n";
    } catch(const std::exception& error) { std::cerr<<"price_ml: "<<error.what()<<'\n';return 1; }
}
