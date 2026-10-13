# Alternative à build.sh : mêmes cibles, mêmes drapeaux, plus ctest.
#   cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release   # -DNATIVE=OFF pour un binaire portable
#   cmake --build build-cmake -j
#   ctest --test-dir build-cmake --output-on-failure        # test_carnet + test_spsc
# Les benchmarks et le simulateur écrivent dans results/ : à lancer depuis la racine du dépôt.
cmake_minimum_required(VERSION 3.16)
project(cpp_market_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NATIVE "Compiler avec -march=native (OFF : binaire portable)" ON)
option(ATTENTE_COOPERATIVE "Attente par std::this_thread::yield() dans test_spsc (utile sous TSan ou sur < 4 coeurs)" OFF)

add_compile_options(-Wall -Wextra)
if(NATIVE)
  add_compile_options(-march=native)
endif()
if(MINGW)
  add_link_options(-static)   # pas de DLL MinGW à trouver au lancement
endif()

find_package(Threads REQUIRED)

add_library(carnet STATIC src/carnet.cpp)
target_include_directories(carnet PUBLIC include)

function(executable nom source)
  add_executable(${nom} ${source})
  target_link_libraries(${nom} PRIVATE carnet Threads::Threads)
endfunction()

executable(test_carnet tests/test_carnet.cpp)
executable(test_spsc tests/test_spsc.cpp)
executable(bench_carnet bench/bench_carnet.cpp)
executable(bench_spsc bench/bench_spsc.cpp)
executable(simulateur src/simulateur.cpp)
executable(mc_pricer src/mc_pricer.cpp)

# Trois threads en attente active n'avancent pas sur moins de 4 coeurs (voir include/pipeline.hpp).
cmake_host_system_information(RESULT NB_COEURS QUERY NUMBER_OF_LOGICAL_CORES)
if(ATTENTE_COOPERATIVE OR NB_COEURS LESS 4)
  target_compile_definitions(test_spsc PRIVATE ATTENTE_COOPERATIVE)
endif()

enable_testing()
add_test(NAME carnet COMMAND test_carnet WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME spsc COMMAND test_spsc WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
