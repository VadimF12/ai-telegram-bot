cmake_minimum_required(VERSION 3.20)
project(untitled5 CXX)

set(CMAKE_CXX_STANDARD 20)

include_directories(/usr/local/include)
link_directories(/usr/local/lib)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

find_library(TGBOT_LIBRARY NAMES TgBot PATHS /usr/local/lib REQUIRED)

add_executable(untitled5 main.cpp)

target_link_libraries(untitled5 PRIVATE
        ${TGBOT_LIBRARY}
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
        nlohmann_json::nlohmann_json
        pthread
        SQLite::SQLite3
)