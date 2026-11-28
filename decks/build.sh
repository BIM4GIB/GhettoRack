#!/bin/sh
# Build the deck engine: build/deckd and build/engine_test.
# On Linux with the ALSA headers (libasound2-dev) deckd plays through a sound
# card; anywhere else it builds with --null only (no sound: tests, the desk).
set -eu
cd "$(dirname "$0")"
OUT="${OUT:-build}"      # install.sh builds straight into the bridge's folder
mkdir -p "$OUT"
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -O2 -Wall -Wextra"
ALSA=""
if [ "$(uname)" = Linux ] && [ -f /usr/include/alsa/asoundlib.h ]; then ALSA="-DHAVE_ALSA -lasound"; fi
[ -n "${DECKD_ONLY:-}" ] || $CXX $FLAGS engine_test.cpp engine.cpp tempo.cpp -o "$OUT/engine_test"
$CXX $FLAGS main.cpp engine.cpp tempo.cpp -o "$OUT/deckd.new" -lpthread $ALSA
mv "$OUT/deckd.new" "$OUT/deckd"
[ -n "$ALSA" ] && echo "built $OUT/deckd (ALSA)" || echo "built $OUT/deckd (no sound card support here: --null only)"
