#!/bin/bash
# Build script for AHV tender (pure C, no ObjC)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_PATH="/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"

echo "Building AHV tender..."

# Compile ahv_core.c
cc -c "$SCRIPT_DIR/tenders/ahv/ahv_core.c" \
   -o "$SCRIPT_DIR/tenders/ahv/ahv_core.o" \
   -I"$SCRIPT_DIR/tenders/ahv" \
   -I"$SCRIPT_DIR/include" \
   -isystem "$SDK_PATH/usr/include" \
   -std=c11 \
   -O2 \
   -g \
   -Wall

# Compile ahv_main.c
cc -c "$SCRIPT_DIR/tenders/ahv/ahv_main.c" \
   -o "$SCRIPT_DIR/tenders/ahv/ahv_main.o" \
   -I"$SCRIPT_DIR/tenders/ahv" \
   -I"$SCRIPT_DIR/include" \
   -isystem "$SDK_PATH/usr/include" \
   -std=c11 \
   -O2 \
   -g \
   -Wall

# Link
cc "$SCRIPT_DIR/tenders/ahv/ahv_core.o" \
   "$SCRIPT_DIR/tenders/ahv/ahv_main.o" \
   -o "$SCRIPT_DIR/tenders/ahv/solo5-ahv" \
   -O2 \
   -g

rm -f "$SCRIPT_DIR/tenders/ahv/ahv_core.o" "$SCRIPT_DIR/tenders/ahv/ahv_main.o"

echo "Built: $SCRIPT_DIR/tenders/ahv/solo5-ahv"