#include "price_model.h"
#include <sqlite3.h>
#include <memory>
#include <stdexcept>
namespace price_ml {
std::vector<Row> readRows(const std::string& path) {
    sqlite3* raw=nullptr;
    const int opened=sqlite3_open_v2(path.c_str(),&raw,SQLITE_OPEN_READONLY,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    if(opened!=SQLITE_OK) throw std::runtime_error("Cannot open source database read-only");
    if(sqlite3_db_readonly(db.get(),"main")!=1) throw std::runtime_error("Source is not read-only");
    sqlite3_busy_timeout(db.get(),1000);
    if(sqlite3_exec(db.get(),"PRAGMA foreign_keys=ON; PRAGMA query_only=ON;",nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("Cannot configure read-only connection");
    // Только один SELECT: согласованный снимок, без миграций, генератора и записи просмотров.
    const char* sql=R"SQL(
      SELECT p.id,p.area,p.rooms,p.floor,p.kind,p.district,p.renovation,l.price,l.deal_type
      FROM properties p JOIN listings l ON l.property_id=p.id
      WHERE p.demo_key=printf('demo-%03d',CAST(substr(p.demo_key,6) AS INTEGER))
        AND CAST(substr(p.demo_key,6) AS INTEGER) BETWEEN 1 AND 1000
        AND l.demo_key=p.demo_key AND l.seller_user_id IS NULL AND l.source_booking_id IS NULL
      ORDER BY p.id,l.id
    )SQL";
    sqlite3_stmt* stmt=nullptr;
    const int prepared=sqlite3_prepare_v2(db.get(),sql,-1,&stmt,nullptr);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(stmt,sqlite3_finalize);
    if(prepared!=SQLITE_OK) throw std::runtime_error("Cannot select original offers; requires project schema 6");
    auto text=[&](int column){const auto p=sqlite3_column_text(stmt,column);return p?std::string(reinterpret_cast<const char*>(p)):std::string();};
    std::vector<Row> rows; int status;
    while((status=sqlite3_step(stmt))==SQLITE_ROW) {
        rows.push_back({sqlite3_column_int64(stmt,0),
            {sqlite3_column_double(stmt,1),sqlite3_column_double(stmt,2),sqlite3_column_double(stmt,3)},
            {text(4),text(5),text(6)},sqlite3_column_double(stmt,7),text(8)});
    }
    if(status!=SQLITE_DONE) throw std::runtime_error("Failed to read database snapshot");
    return rows;
}
}
