#!/usr/bin/env bash
# Cross-builds the AmigaOS 4 version in Docker (GCC 15 for AmigaOS 4,
# newlib and SDL 3, from the walkero/amigagccondocker:os4-gcc15 image).
#
# Usage: bash tools/build_os4.sh [Release|Debug]
# Runs on Windows (MSYS2 or Git Bash), Linux and macOS with Docker.
# Output: build-os4/supremacy (with symbols, for reading crash logs) and
# build-os4/supremacy.stripped (the one to ship), plus build-os4/supremacy.info.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TYPE="${1:-Release}"
IMAGE="${OS4_IMAGE:-walkero/amigagccondocker:os4-gcc15}"

# The version banner's commit (cmake/build_id.cmake; git does not run in the container)
if BID="$(git -C "$ROOT" rev-parse --short=8 HEAD 2>/dev/null)"; then
  git -C "$ROOT" diff --quiet HEAD || BID="$BID-dirty"
  BDATE="$(git -C "$ROOT" log -1 --format=%cd --date=format:%d.%m.%Y)"
else
  BID=unknown
  BDATE=""
fi

# Docker on Windows wants a Windows path; elsewhere, build as the current
# user so the output files are not owned by root.
if command -v cygpath >/dev/null 2>&1; then
  HOSTDIR="$(cygpath -w "$ROOT")"
  USEROPT=()
else
  HOSTDIR="$ROOT"
  USEROPT=(--user "$(id -u):$(id -g)")
fi

MSYS_NO_PATHCONV=1 docker run --rm "${USEROPT[@]}" -e SUP_BUILD_ID="$BID" -e SUP_BUILD_DATE="$BDATE" \
  -v "$HOSTDIR:/work" -w /work "$IMAGE" sh -c "
  cmake -S . -B build-os4 -DCMAKE_TOOLCHAIN_FILE=cmake/amigaos4-gcc15.cmake -DCMAKE_BUILD_TYPE=$TYPE &&
  cmake --build build-os4 -j4 &&
  ppc-amigaos-strip -o build-os4/supremacy.stripped build-os4/supremacy"
