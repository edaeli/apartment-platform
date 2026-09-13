#include "seed.h"
#include "legacy_seed_fixture.h"
#include <iostream>
#include <set>
#include <map>

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
using Rows = std::vector<std::vector<std::string>>;
Rows rows(Database& db, const std::string& sql) {
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(db.handle(),sql.c_str(),-1,&q,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db.handle()));
    Rows result;
    int rc;
    while ((rc=sqlite3_step(q))==SQLITE_ROW) {
        std::vector<std::string> row;
        for (int i=0;i<sqlite3_column_count(q);++i) {
            const auto value=sqlite3_column_text(q,i);row.push_back(value?reinterpret_cast<const char*>(value):"<NULL>");
        }
        result.push_back(row);
    }
    sqlite3_finalize(q);
    if(rc!=SQLITE_DONE) throw std::runtime_error("Read failed");
    return result;
}
std::map<std::string,Rows> snapshot(Database& db) {
    std::map<std::string,Rows> result;
    for (const auto* t : {"properties","listings","property_photos","users","bookings","favorites","listing_views"})
        result[t]=rows(db,std::string("SELECT * FROM ")+t+" ORDER BY rowid");
    return result;
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"schema required");
        Database first(":memory:",true),second(":memory:",true);
        first.migrate(argv[1]);second.migrate(argv[1]);seedDemoData(first);seedDemoData(second);
        check(rows(first,"SELECT * FROM properties ORDER BY id")==rows(second,"SELECT * FROM properties ORDER BY id"),"Reproducibility");
        check(rows(first,"SELECT id,property_id,demo_key,deal_type,price FROM listings ORDER BY id")==rows(second,"SELECT id,property_id,demo_key,deal_type,price FROM listings ORDER BY id"),"Price reproducibility");
        const auto stable=snapshot(first);seedDemoData(first);refreshDemoData(first,true);check(snapshot(first)==stable,"Ordinary seed/repeated update changed new data");
        for(int n=1;n<=1000;++n) {
            const auto p=demoProperty(n);const bool house=p.kind=="house";
            check(p.rooms>=(house?3:1) && p.rooms<=(house?7:5),"Room range");
            check(p.area>=(house?p.rooms*25+30:p.rooms*18+10) && p.area<=(house?p.rooms*25+90:p.rooms*18+35),"Area/rooms inconsistent");
            check(house?p.floor==0:p.floor>=1 && p.floor<=16,"Floor inconsistent");
            check(p.price>0 && p.price%(p.dealType=="rent"?1000:50000)==0,"Rounding invalid");
            const auto base=demoPrice(p.area,p.district,p.kind,p.dealType,p.renovation);
            const auto rounding=p.dealType=="rent"?1000:50000;
            check(p.price>=base*.92-rounding && p.price<=base*1.08+rounding,"Noise outside limit");
            if(p.renovation=="needs_repair") check(p.description.find("Нужны восстановительные")!=std::string::npos && p.description.find("дизайнерский")==std::string::npos,"Repair description contradiction");
        }
        for (const auto* deal:{"rent","sale"}) {
            std::int64_t previous=0;
            for (const auto* repair:{"needs_repair","cosmetic","good","designer"}) {
                const auto price=demoPrice(80,"Кентрон","apartment",deal,repair);
                check(price>previous,"Repair should increase equal-property baseline");previous=price;
            }
        }
        check(demoPrice(80,"Кентрон","apartment","rent","good")==400000,"Known rent calculation");
        check(demoPrice(80,"Кентрон","apartment","sale","good")==68000000,"Known sale calculation");
        check(first.scalar("SELECT COUNT(DISTINCT area) FROM properties")>600,"Area diversity too low");
        check(first.scalar("SELECT COUNT(DISTINCT price) FROM listings WHERE deal_type='rent'")>250,"Rent diversity too low");
        check(first.scalar("SELECT COUNT(DISTINCT price) FROM listings WHERE deal_type='sale'")>400,"Sale diversity too low");

        Database old(":memory:",true);old.migrate(argv[1]);legacyFixture(old,30);
        check(old.listing(1)->price==280000 && old.listing(1)->area==64,"Legacy anchor");
        const auto u=old.createUser("Fixture","fixture@example.com","not a real hash");
        old.releaseBooking(u,old.book(u,1));
        const auto resale=old.resellBooking(u,old.book(u,2),55000000);
        old.book(u,3);old.setFavorite(u,4,true);old.recordView(u,5);
        old.execute("UPDATE listings SET price=1 WHERE id=6");
        old.execute("UPDATE properties SET area=99 WHERE id=7");
        old.execute("UPDATE properties SET description='User text' WHERE id=8");
        old.execute("UPDATE property_photos SET caption='User photo' WHERE property_id=9");
        old.execute("UPDATE listings SET demo_key=NULL WHERE id=10");
        old.execute("UPDATE properties SET demo_revision=2 WHERE id=11");
        old.execute("DELETE FROM listings WHERE id=12");
        old.execute("UPDATE properties SET address='User address' WHERE id=13");
        old.execute("UPDATE properties SET renovation='good' WHERE id=14");
        old.execute("UPDATE listings SET status='closed' WHERE id=15");
        old.execute("UPDATE properties SET longitude=44 WHERE id=16");
        old.execute("INSERT INTO property_photos(property_id,url,caption,sort_order) VALUES(17,'/custom.jpg','Custom',3)");
        const auto before=snapshot(old);
        const auto preview=refreshDemoData(old,false);
        check(preview.eligible==13 && preview.protectedCount==6 && preview.changed==10 && preview.alreadyCurrent==1 && preview.missing==970,"Wrong preview classifications");
        check(snapshot(old)==before,"Preview changed rows");
        // Ошибка между UPDATE недвижимости и цены должна откатить весь пакет.
        old.execute("CREATE TRIGGER fail_demo BEFORE UPDATE OF price ON listings WHEN OLD.id=20 BEGIN SELECT RAISE(ABORT,'test failure'); END");
        bool failed=false;try {refreshDemoData(old,true);}catch(const SqliteError&) {failed=true;}
        check(failed && snapshot(old)==before,"Refresh did not roll back full batch");
        old.execute("DROP TRIGGER fail_demo");
        const auto applied=refreshDemoData(old,true);check(applied.eligible==13,"Preview/apply mismatch");
        const auto after=snapshot(old);
        for (const auto* t:{"users","bookings","favorites","listing_views","property_photos"}) check(after.at(t)==before.at(t),"User data/photos changed");
        for (std::size_t i=0;i<17;++i) check(after.at("properties")[i]==before.at("properties")[i],"Protected property changed");
        for (std::size_t i=0;i<before.at("listings").size();++i) {
            const auto id=std::stoll(before.at("listings")[i][0]);
            if (id<18 || id>30) check(after.at("listings")[i]==before.at("listings")[i],"Protected/history listing changed");
        }
        check(old.listing(resale)->price==55000000 && old.listing(resale)->renovation=="unspecified","User resale changed");
        check(old.listing(18)->renovation!="unspecified","Renovation missing in listing reader");
        check(refreshDemoData(old,true).eligible==0 && snapshot(old)==after,"Second refresh changed rows");
        old.execute("UPDATE properties SET description='Changed after refresh' WHERE id=18");
        const auto edited=snapshot(old);refreshDemoData(old,true);check(snapshot(old)==edited,"User edits after refresh lost");
        bool invalid=false;try{old.execute("UPDATE properties SET renovation='random' WHERE id=18");}catch(const SqliteError&){invalid=true;}
        check(invalid,"Unknown repair accepted");
        std::cout<<"PASS: deterministic demo, coherent attributes, pricing factors, diversity, dry run, protected/edited data, full rollback and idempotence\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
