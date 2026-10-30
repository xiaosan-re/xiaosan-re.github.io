#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LLVM=${LLVM_PREFIX:-/opt/homebrew/opt/llvm@21}
"$LLVM/bin/clang++" -std=c++17 -fPIC -shared -fno-rtti "$HERE/BusinessVM.cpp" $("$LLVM/bin/llvm-config" --cxxflags --ldflags --libs core passes support) -o "$HERE/BusinessVM.dylib"
