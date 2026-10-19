cmake_minimum_required(VERSION 3.20)
project(nanoexchange VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NX_BUILD_TESTS "Build unit tests" ON)
option(NX_BUILD_BENCH "Build benchmark" ON)
set(NX_SANITIZE "" CACHE STRING "Sanitizer to enable: address | thread | empty")

add_compile_options(-Wall -Wextra -Wpedantic)

if(NX_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(NX_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
find_package(Boost 1.74 REQUIRED)             # Asio + Beast are header-only
find_package(nlohmann_json 3.10 REQUIRED)

# ---- engine: pure C++, no networking -------------------------------------
add_library(nx_engine
  src/engine/order_book.cpp
  src/engine/wal.cpp
  src/engine/symbol_engine.cpp
)
target_include_directories(nx_engine PUBLIC src)
target_link_libraries(nx_engine PUBLIC Threads::Threads)

# ---- server: Boost.Beast WebSocket + HTTP --------------------------------
add_executable(nanoexchange
  src/main.cpp
  src/server/hub.cpp
  src/server/protocol.cpp
  src/server/server.cpp
  src/server/bots.cpp
  src/server/metrics_http.cpp
)
target_link_libraries(nanoexchange PRIVATE nx_engine Boost::headers nlohmann_json::nlohmann_json)
target_compile_definitions(nanoexchange PRIVATE BOOST_ASIO_NO_DEPRECATED)

# ---- tests ----------------------------------------------------------------
if(NX_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(nx_tests
    tests/order_book_test.cpp
    tests/wal_recovery_test.cpp
    tests/engine_thread_test.cpp
  )
  target_link_libraries(nx_tests PRIVATE nx_engine GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(nx_tests)
endif()

# ---- benchmark ------------------------------------------------------------
if(NX_BUILD_BENCH)
  add_executable(nx_bench bench/bench_engine.cpp)
  target_link_libraries(nx_bench PRIVATE nx_engine)
endif()
