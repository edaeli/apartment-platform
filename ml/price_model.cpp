#include "price_model.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
namespace price_ml {
namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void validateRow(const Row& r) {
    require(r.propertyId > 0 && std::isfinite(r.price) && r.price > 0, "Invalid row/id/price");
    for (double x : r.numeric) require(std::isfinite(x), "Non-finite feature");
    require(r.numeric[0]>0 && r.numeric[1]>0 && r.numeric[2]>=0, "Invalid property features");
    require(r.deal=="rent" || r.deal=="sale", "Unknown deal");
}
// SplitMix64: фиксированная арифметика uint64_t, не зависящая от std::hash/RNG STL.
std::uint64_t hashId(std::uint64_t id) {
    auto x = id + seed + UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
// Решаем min ||A*w-b|| через отражения Хаусхолдера. Не строим A^T A или inverse.
std::vector<double> solveQR(std::vector<std::vector<double>> a, std::vector<double> b) {
    const auto m=a.size(), n=a.front().size();
    require(m>=n && b.size()==m, "Invalid QR dimensions");
    for (std::size_t k=0;k<n;++k) {
        double norm=0;
        for (std::size_t i=k;i<m;++i) norm=std::hypot(norm,a[i][k]);
        require(norm>1e-12 && std::isfinite(norm), "Rank deficient QR");
        const double alpha=-std::copysign(norm,a[k][k]);
        std::vector<double> v(m-k);
        for (std::size_t i=k;i<m;++i) v[i-k]=a[i][k];
        v[0]-=alpha;
        double vnorm=0;
        for (double x:v) vnorm=std::hypot(vnorm,x);
        for (double& x:v) x/=vnorm;
        for (std::size_t j=k;j<n;++j) {
            double dot=0;
            for (std::size_t i=k;i<m;++i) dot+=v[i-k]*a[i][j];
            for (std::size_t i=k;i<m;++i) a[i][j]-=2*v[i-k]*dot;
        }
        double dot=0;
        for (std::size_t i=k;i<m;++i) dot+=v[i-k]*b[i];
        for (std::size_t i=k;i<m;++i) b[i]-=2*v[i-k]*dot;
    }
    std::vector<double> w(n);
    for (std::size_t i=n;i-->0;) {
        double value=b[i];
        for (std::size_t j=i+1;j<n;++j) value-=a[i][j]*w[j];
        w[i]=value/a[i][i];
        require(std::isfinite(w[i]), "Non-finite coefficient");
    }
    return w;
}
}
Split splitRows(const std::vector<Row>& rows, const std::string& deal) {
    Split result;
    std::set<std::int64_t> ids;
    for (const auto& row:rows) {
        validateRow(row);
        if (row.deal!=deal) continue;
        require(ids.insert(row.propertyId).second, "Duplicate property in original offers");
        (hashId(static_cast<std::uint64_t>(row.propertyId))%5==0 ? result.test:result.train).push_back(row);
    }
    auto order=[](const Row& a,const Row& b){return a.propertyId<b.propertyId;};
    std::sort(result.train.begin(),result.train.end(),order);
    std::sort(result.test.begin(),result.test.end(),order);
    return result;
}
std::vector<std::string> Model::featureNames() const {
    std::vector<std::string> names={"intercept","area_z","rooms_z","floor_z"};
    const std::array<std::string,3> prefixes={"kind","district","renovation"};
    for (std::size_t j=0;j<3;++j) {
        for (const auto& value:categories[j]) names.push_back(prefixes[j]+"="+value);
        names.push_back(prefixes[j]+"=<unknown>");
    }
    return names;
}
std::vector<double> Model::encode(const Row& r) const {
    std::vector<double> x={1};
    for (std::size_t j=0;j<3;++j) {
        require(std::isfinite(r.numeric[j]) && scale[j]>0, "Invalid numeric feature/scale");
        x.push_back((r.numeric[j]-mean[j])/scale[j]);
    }
    for (std::size_t j=0;j<3;++j) {
        bool found=false;
        for (const auto& category:categories[j]) {
            const bool match=r.category[j]==category;
            x.push_back(match?1:0); found=found||match;
        }
        x.push_back(found?0:1);
    }
    return x;
}
double Model::predict(const Row& row) const {
    require(row.deal==deal, "Use separate model for this deal");
    const auto x=encode(row);
    require(x.size()==coefficients.size(), "Invalid model dimensions");
    double prediction=targetMean+targetScale*std::inner_product(x.begin(),x.end(),coefficients.begin(),0.0);
    require(std::isfinite(prediction), "Non-finite prediction");
    return std::max(1.0,prediction); // Явное ограничение цены, одинаковое при оценке и загрузке.
}
Model train(const std::vector<Row>& rows,const std::string& deal) {
    require(rows.size()>=2, "Need at least two training objects");
    Model m; m.deal=deal;
    for (const auto& r:rows) { validateRow(r); require(r.deal==deal,"Mixed deals in training"); }
    std::vector<double> prices;
    for (std::size_t j=0;j<3;++j) {
        std::set<std::string> categories;
        for (const auto& r:rows) { m.mean[j]+=r.numeric[j]/rows.size(); categories.insert(r.category[j]); }
        double sum=0;
        for (const auto& r:rows) sum+=std::pow(r.numeric[j]-m.mean[j],2);
        m.scale[j]=std::sqrt(sum/rows.size());
        if (m.scale[j]<1e-12) m.scale[j]=1;
        m.categories[j]={categories.begin(),categories.end()};
    }
    for (const auto& r:rows) { prices.push_back(r.price); m.targetMean+=r.price/rows.size(); }
    double sum=0;
    for (double p:prices) sum+=std::pow(p-m.targetMean,2);
    m.targetScale=std::sqrt(sum/rows.size());
    if(m.targetScale<1e-12) m.targetScale=1;
    std::sort(prices.begin(),prices.end());
    m.baseline=(prices[(prices.size()-1)/2]+prices[prices.size()/2])/2;
    std::vector<std::vector<double>> a;
    std::vector<double> b;
    for (const auto& r:rows) { a.push_back(m.encode(r)); b.push_back((r.price-m.targetMean)/m.targetScale); }
    const auto n=a.front().size();
    // sqrt(lambda)*I добавляет L2-штраф. Свободный член (столбец 0) не штрафуется.
    for (std::size_t j=1;j<n;++j) {
        std::vector<double> penalty(n,0); penalty[j]=std::sqrt(m.lambda);
        a.push_back(penalty); b.push_back(0);
    }
    m.coefficients=solveQR(a,b);
    return m;
}
double mae(const Model& model,const std::vector<Row>& rows,bool baseline) {
    require(!rows.empty(),"Empty evaluation set");
    double result=0;
    for (const auto& row:rows) result+=std::abs((baseline?model.baseline:model.predict(row))-row.price)/rows.size();
    return result;
}
void Model::save(const std::string& path) const {
    std::ofstream out(path);
    require(bool(out),"Cannot write model");
    out<<std::setprecision(std::numeric_limits<double>::max_digits10);
    out<<"APARTMENT_RIDGE 1\n"<<std::quoted(deal)<<' '<<splitSeed<<' '<<lambda<<'\n';
    out<<"target_standardized_price_amd_min_1\n"<<targetMean<<' '<<targetScale<<' '<<baseline<<'\n';
    for (std::size_t j=0;j<3;++j) {
        out<<mean[j]<<' '<<scale[j]<<' '<<categories[j].size();
        for (const auto& c:categories[j]) out<<' '<<std::quoted(c);
        out<<'\n';
    }
    const auto names=featureNames();
    out<<coefficients.size()<<'\n';
    for (std::size_t j=0;j<coefficients.size();++j) out<<std::quoted(names.at(j))<<' '<<coefficients[j]<<'\n';
    out.close(); require(bool(out),"Failed to write model");
}
Model Model::load(const std::string& path) {
    std::ifstream in(path); Model m; std::string magic,target; int version=0;
    in>>magic>>version;
    require(magic=="APARTMENT_RIDGE" && version==1,"Unsupported model format");
    in>>std::quoted(m.deal)>>m.splitSeed>>m.lambda>>target>>m.targetMean>>m.targetScale>>m.baseline;
    require((m.deal=="rent"||m.deal=="sale") && m.splitSeed==seed && m.lambda==ridge &&
        target=="target_standardized_price_amd_min_1", "Invalid model metadata");
    for (std::size_t j=0;j<3;++j) {
        std::size_t count=0; in>>m.mean[j]>>m.scale[j]>>count;
        require(count<=1000,"Too many categories");
        m.categories[j].resize(count);
        for (auto& c:m.categories[j]) in>>std::quoted(c);
        require(std::isfinite(m.mean[j]) && std::isfinite(m.scale[j]) && m.scale[j]>0,"Invalid scaling");
        require(std::is_sorted(m.categories[j].begin(),m.categories[j].end()) &&
          std::adjacent_find(m.categories[j].begin(),m.categories[j].end())==m.categories[j].end(),"Invalid categories");
    }
    auto names=m.featureNames(); std::size_t count=0; in>>count;
    require(count==names.size(),"Invalid coefficient count"); m.coefficients.resize(count);
    for (std::size_t j=0;j<count;++j) {
        std::string name; in>>std::quoted(name)>>m.coefficients[j];
        require(name==names[j] && std::isfinite(m.coefficients[j]),"Invalid feature/coefficient");
    }
    require(bool(in) && std::isfinite(m.targetMean) && std::isfinite(m.targetScale) && m.targetScale>0 &&
      std::isfinite(m.baseline) && m.baseline>0,"Invalid/truncated model");
    in>>std::ws; require(in.eof(),"Trailing model data");
    return m;
}
}
