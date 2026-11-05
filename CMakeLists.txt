cmake_minimum_required(VERSION 3.16)
project(mdp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(FetchContent)
set(SIMDJSON_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG tags/v4.6.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(simdjson)

# Portable core: book, parser, synchronization state machine.
add_library(mdp_core src/order_book.cpp src/sync_engine.cpp)
target_include_directories(mdp_core PUBLIC include)
target_link_libraries(mdp_core PUBLIC simdjson::simdjson)

# CLI: replay everywhere; live (WinHTTP) on Windows only.
add_executable(mdp src/main.cpp)
target_link_libraries(mdp PRIVATE mdp_core)
if(WIN32)
  target_sources(mdp PRIVATE src/exchange_client.cpp)
  target_link_libraries(mdp PRIVATE winhttp)
endif()

enable_testing()
foreach(t test_order_book test_sync_engine)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE mdp_core)
  target_compile_definitions(${t} PRIVATE MDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(replay_bench bench/replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE mdp_core)
