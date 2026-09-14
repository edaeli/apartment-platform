#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace price_ml {
constexpr std::uint64_t seed = 20260914;
constexpr double ridge = 1.0; // Фиксировано ДО оценки на тесте.
struct Row {
    std::int64_t propertyId;
    std::array<double,3> numeric; // area, rooms, floor
    std::array<std::string,3> category; // kind, district, renovation
    double price;
    std::string deal;
};
struct Split { std::vector<Row> train, test; };
struct Model {
    std::string deal;
    std::uint64_t splitSeed = seed;
    double lambda = ridge;
    std::array<double,3> mean{}, scale{};
    std::array<std::vector<std::string>,3> categories;
    double targetMean = 0, targetScale = 1, baseline = 0;
    std::vector<double> coefficients;
    std::vector<std::string> featureNames() const;
    std::vector<double> encode(const Row& row) const;
    double predict(const Row& row) const;
    void save(const std::string& path) const;
    static Model load(const std::string& path);
};
std::vector<Row> readRows(const std::string& dbPath);
Split splitRows(const std::vector<Row>& rows, const std::string& deal);
Model train(const std::vector<Row>& rows, const std::string& deal);
double mae(const Model& model, const std::vector<Row>& rows, bool baseline = false);
}
