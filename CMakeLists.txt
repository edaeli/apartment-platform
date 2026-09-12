cmake_minimum_required(VERSION 3.20)
project(apartment_platform VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Сначала стандартный модуль CMake: Drogon добавляет свой FindSQLite3.cmake.
find_package(SQLite3 3.24 REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY sodium REQUIRED)

add_library(apartment_db src/database.cpp src/seed.cpp src/accounts.cpp)
target_include_directories(apartment_db PUBLIC src)
if(TARGET SQLite3::SQLite3)
    target_link_libraries(apartment_db PUBLIC SQLite3::SQLite3)
else()
    target_link_libraries(apartment_db PUBLIC SQLite::SQLite3)
endif()

add_library(apartment_security src/security.cpp)
target_include_directories(apartment_security PUBLIC src PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(apartment_security PUBLIC ${SODIUM_LIBRARY})

add_executable(apartment_server src/main.cpp src/http_helpers.cpp src/auth.cpp)
target_link_libraries(apartment_server PRIVATE apartment_db apartment_security Drogon::Drogon)

foreach(target apartment_db apartment_security apartment_server)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(database_tests tests/database_tests.cpp)
    target_link_libraries(database_tests PRIVATE apartment_db)
    add_test(NAME database COMMAND database_tests ${PROJECT_SOURCE_DIR}/sql/001_initial.sql)

    add_executable(security_tests tests/security_tests.cpp)
    target_link_libraries(security_tests PRIVATE apartment_security)
    add_test(NAME security COMMAND security_tests)
    add_executable(booking_tests tests/booking_tests.cpp)
    target_link_libraries(booking_tests PRIVATE apartment_db)
    add_test(NAME booking_database COMMAND booking_tests ${PROJECT_SOURCE_DIR}/sql/001_initial.sql)

    # Python используется только для проверки HTTP, не для бизнес-логики.
    find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
    add_test(NAME http_smoke COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/http_smoke.py
        $<TARGET_FILE:apartment_server> ${PROJECT_SOURCE_DIR})
    add_test(NAME auth_http COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/auth_http.py
        $<TARGET_FILE:apartment_server> ${PROJECT_SOURCE_DIR})
    set_tests_properties(http_smoke auth_http PROPERTIES TIMEOUT 60)
endif()
