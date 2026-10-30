#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LLVM=${LLVM_PREFIX:-/opt/homebrew/opt/llvm@21}
PASS_DIR="$HERE/../../passes/business-vm"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/quote-walkthrough.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
sh "$PASS_DIR/build.sh"
"$LLVM/bin/clang" -O1 -emit-llvm -c -DQUOTE_NAME=quote_protected "$HERE/quote.c" -o "$TMP/input.bc"
OMVLL_VM_SEED=0x2468ace1 "$LLVM/bin/opt" -load-pass-plugin="$PASS_DIR/BusinessVM.dylib" \
  -passes=v4-virtualize "$TMP/input.bc" -o "$TMP/vm.bc"
"$LLVM/bin/clang" -O1 -shared -fPIC "$HERE/quote.c" "$TMP/vm.bc" -o "$TMP/quote.dylib"
python3 "$HERE/verify.py" "$TMP/quote.dylib" "$TMP/verification.json"
# Native macOS arm64 validation only; the historical blind experiment is separate.
