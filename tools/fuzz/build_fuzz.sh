#!/bin/sh
# Builds the test programs in tools/fuzz with AddressSanitizer and
# UndefinedBehaviorSanitizer, for Linux (or any system with gcc or clang
# that supports the sanitizers). They need no SDL and open no window.
#
#   fuzz_game     plays whole games with random input and reports freezes
#   fuzz_extract  feeds damaged archives and disk images to the extractor
#   test_evwait   checks the one event that waits for a YES/NO answer
#   test_shows    runs the intro, the five end sequences and their music
#
# Run it from the repository root:
#   sh tools/fuzz/build_fuzz.sh
# On Windows, run it in Docker (any Linux image with gcc works), e.g.:
#   docker run --rm -v "%CD%":/work -w /work gcc:14 sh tools/fuzz/build_fuzz.sh
#
# The programs are written to build-fuzz/. Each needs the game's data
# folder (see README.md); usage is at the top of each source file.
set -e
CC="${FUZZ_CC:-gcc}"   # FUZZ_CC, not CC: some images point CC at a cross compiler
FLAGS="-std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
       -fno-sanitize-recover=undefined -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter"
INC="-Isrc/core -Isrc/ui -Isrc/assets -Isrc/sound"
mkdir -p build-fuzz

$CC $FLAGS $INC tools/fuzz/fuzz_game.c src/core/*.c src/ui/*.c src/assets/gamedata.c \
    src/sound/sequencer.c src/sound/paula.c -o build-fuzz/fuzz_game -lm
$CC $FLAGS $INC tools/fuzz/test_evwait.c src/core/*.c src/ui/*.c src/assets/gamedata.c \
    -o build-fuzz/test_evwait -lm
$CC $FLAGS $INC tools/fuzz/test_shows.c src/ui/intro.c src/ui/ending.c src/sound/*.c src/core/*.c \
    src/assets/gamedata.c -o build-fuzz/test_shows -lm
# the extractor without SDL: tools/fuzz/shim stands in for the few SDL calls it makes
$CC $FLAGS -Itools/fuzz/shim -Isrc/platform tools/fuzz/fuzz_extract.c src/platform/extract.c \
    -o build-fuzz/fuzz_extract
echo "built build-fuzz/fuzz_game, test_evwait, test_shows, fuzz_extract"
