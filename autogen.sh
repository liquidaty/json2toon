#!/bin/sh
# json2toon ships a handwritten ./configure (no autoconf/automake required).
# This script exists for parity with the documented git-checkout workflow; it
# simply makes the build scripts executable and points at ./configure.
set -e
chmod +x configure 2>/dev/null || true
echo "autogen: ./configure is ready to run."
