#!/usr/bin/env bash
# Builds the kjs engine: the kjs command line (test262 runner) and libkjs.a for the browser.
set -e
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-g++}
FLAGS="-std=c++20 -O2 -Wall -Wno-unused-parameter -Wno-unused-function"
SRCS="src/lexer.cpp src/parser.cpp src/compiler.cpp src/interp.cpp src/runtime.cpp src/builtins.cpp src/array.cpp src/string.cpp src/promise.cpp src/regexp.cpp"
OBJS=""
for s in $SRCS; do
  o=build/$(basename "${s%.cpp}").o
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ -n "$(find src -name '*.h' -newer "$o")" ]; then
    $CXX $FLAGS -c "$s" -o "$o" &
  fi
  OBJS="$OBJS $o"
done
wait
ar rcs build/libkjs.a $OBJS
LINK=""
case "$(uname -s)" in MINGW*|MSYS*) LINK="-static -Wl,--stack,268435456";; *) LINK="-pthread";; esac
$CXX $FLAGS src/main.cpp build/libkjs.a -o build/kjs $LINK
echo built js/build/kjs
