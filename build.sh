#!/bin/bash
#
# Build foal-httpd for SymbOS
#
# Requires: SCC (SymbOS C Compiler)
# Set SCC_HOME to the scc root, or it defaults to ~/Dev/scc
#
SCC_HOME="${SCC_HOME:-$HOME/Dev/scc}"
CC="$SCC_HOME/bin/cc"
OUTPUT_DIR=./build

if [ ! -x "$CC" ]; then
    echo "Error: cc not found at $CC"
    echo "Set SCC_HOME to your scc installation directory"
    exit 1
fi

echo "Building foal-httpd..."
mkdir -p "$OUTPUT_DIR"

"$CC" src/httpd.c \
    -lnet \
    -N "foal-httpd" \
    -o "$OUTPUT_DIR/httpd.com" \
    || exit 1

echo ""
echo "Build successful: $OUTPUT_DIR/httpd.com"
ls -lh "$OUTPUT_DIR/httpd.com"
