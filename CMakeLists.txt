cmake_minimum_required(VERSION 3.10)

project(trading_engine)

# Specify C++11 or higher
set(CMAKE_CXX_STANDARD 11)
set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
set(OPENSSL_INCLUDE_DIR /opt/homebrew/opt/openssl@3/include)
set(OPENSSL_LIBRARIES /opt/homebrew/opt/openssl@3/lib)

# Add WebSocket++ to include path
include_directories(libs)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost 1.70 REQUIRED COMPONENTS system thread filesystem)
find_package(OpenSSL REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# Add the main.cpp source file
add_executable(trading_engine src/main.cpp)

# Link required libraries
# target_link_libraries(trading_engine pthread)
target_link_libraries(trading_engine pthread ${Boost_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
