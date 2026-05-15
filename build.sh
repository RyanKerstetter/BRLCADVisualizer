#!/usr/bin/env bash

set -e

ROOT="/Users/morrison/IBRT"
BUILD_ROOT="$ROOT/.build"
DEPS_PREFIX="/Users/morrison/bext/.build/install"
BRLCAD_PREFIX="/Users/morrison/brlcad.main/.build"

cmake -S "$ROOT/brl_cad_standalone" \
  -B "$BUILD_ROOT/brl_cad_standalone" \
  -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" \
  -DOSPRAY_PREFIX="$DEPS_PREFIX" \
  -DRKCOMMON_PREFIX="$DEPS_PREFIX" \
  -DEMBREE_PREFIX="$DEPS_PREFIX" \
  -DOPENVKL_PREFIX="$DEPS_PREFIX" \
  -DBRLCAD_PREFIX="$BRLCAD_PREFIX"

cmake --build "$BUILD_ROOT/brl_cad_standalone"
cmake --install "$BUILD_ROOT/brl_cad_standalone"

cmake -S "$ROOT/QtOsprayViewer/QtOsprayViewer" \
  -B "$BUILD_ROOT/QtOsprayViewer" \
  -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" \
  -DQt6_DIR="$DEPS_PREFIX/lib/cmake/Qt6" \
  -DOSPRAY_PREFIX="$DEPS_PREFIX" \
  -DRKCOMMON_PREFIX="$DEPS_PREFIX" \
  -DBRLCAD_PREFIX="$BRLCAD_PREFIX" \
  -DIBRT_ENABLE_RENDER_WORKER=ON

cmake --build "$BUILD_ROOT/QtOsprayViewer"
ctest --test-dir "$BUILD_ROOT/QtOsprayViewer"

"$BUILD_ROOT/QtOsprayViewer/IBRT"
