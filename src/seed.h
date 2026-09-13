#pragma once
#include "database.h"

struct DemoProperty {
    std::string key;
    std::string kind;
    std::string address;
    std::string district;
    double area;
    int rooms;
    int floor;
    std::string dealType;
    std::int64_t price;
    std::string description;
    double latitude;
    double longitude;
    std::string renovation = "unspecified";
};

// Старый эталон нужен только для распознавания неизменённых исходных записей.
DemoProperty legacyDemoProperty(int number);
DemoProperty demoProperty(int number);
std::int64_t demoPrice(double area, const std::string& district, const std::string& kind,
                       const std::string& deal, const std::string& renovation, int deviationPercent = 0);
struct DemoRefreshResult { int eligible = 0, protectedCount = 0, changed = 0, alreadyCurrent = 0, missing = 0; };
DemoRefreshResult refreshDemoData(Database& db, bool apply);
