cmake_minimum_required(VERSION 3.10)

project(trading_engine)

set(CMAKE_CXX_STANDARD 20)
set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
set(OPENSSL_INCLUDE_DIR /opt/homebrew/opt/openssl@3/include)
set(OPENSSL_LIBRARIES /opt/homebrew/opt/openssl@3/lib)
set(NLOHMANN_JSON_DIR /opt/homebrew/opt/nlohmann-json)
set(YAML_CPP_DIR /opt/homebrew/opt/yaml-cpp)

link_directories(${YAML_CPP_DIR}/lib)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost 1.70 REQUIRED COMPONENTS system thread filesystem)
find_package(OpenSSL REQUIRED)
find_package(yaml-cpp REQUIRED PATHS /opt/homebrew/opt/yaml-cpp)

include_directories(${Boost_INCLUDE_DIRS})
include_directories(${NLOHMANN_JSON_DIR}/include)
include_directories(${YAML_CPP_DIR}/include)

add_executable(trading_engine src/main.cpp)

target_link_libraries(trading_engine pthread ${Boost_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto yaml-cpp)
