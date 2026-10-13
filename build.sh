#!/usr/bin/env bash
# Compile tout dans build/. Usage : bash build.sh   (CMakeLists.txt : même chose avec CMake et ctest)
#
# Variables d'environnement (toutes facultatives) :
#   CXX     compilateur C++20 (défaut : g++ du PATH, g++ >= 13 requis). Sous Windows/MinGW :
#           CXX=/chemin/vers/g++.exe bash build.sh
#   NATIVE  1 (défaut) = -march=native ; 0 = binaire portable (autre machine que celle du build)
#   CXXEXTRA  drapeaux supplémentaires, ex. CXXEXTRA="-fsanitize=thread" (Linux)
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || [ -x "$CXX" ] || { echo "compilateur introuvable : $CXX (définir CXX=...)" >&2; exit 1; }

FLAGS="-O3 -std=c++20 -Wall -Wextra -Iinclude ${CXXEXTRA:-}"
[ "${NATIVE:-1}" = "1" ] && FLAGS="$FLAGS -march=native"
LINK="-pthread"
EXT=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXT=".exe"; LINK="-pthread -static" ;;  # static : pas de DLL MinGW à trouver au lancement
esac

# Tests de la file SPSC : sur moins de 4 cœurs, 3 threads en attente active n'avancent pas (voir pipeline.hpp).
NCPU="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
TEST_SPSC_FLAGS=""
[ "$NCPU" -lt 4 ] && TEST_SPSC_FLAGS="-DATTENTE_COOPERATIVE"

mkdir -p build results figures
echo "compilateur : $("$CXX" --version | head -1)  |  flags : $FLAGS  |  cœurs : $NCPU"
"$CXX" $FLAGS -c src/carnet.cpp -o build/carnet.o
"$CXX" $FLAGS tests/test_carnet.cpp build/carnet.o -o "build/test_carnet$EXT" $LINK
"$CXX" $FLAGS bench/bench_carnet.cpp build/carnet.o -o "build/bench_carnet$EXT" $LINK
"$CXX" $FLAGS $TEST_SPSC_FLAGS tests/test_spsc.cpp build/carnet.o -o "build/test_spsc$EXT" $LINK
"$CXX" $FLAGS bench/bench_spsc.cpp build/carnet.o -o "build/bench_spsc$EXT" $LINK
"$CXX" $FLAGS src/simulateur.cpp build/carnet.o -o "build/simulateur$EXT" $LINK
"$CXX" $FLAGS src/mc_pricer.cpp -o "build/mc_pricer$EXT" $LINK
echo "OK -> build/"
