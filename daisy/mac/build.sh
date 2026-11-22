#!/bin/sh
# Builds the Mac version of ApcBox. Needs only the Xcode command line
# tools (the same clang you already have). Run from anywhere:
#   ~/dev/DaisyExamples/seed/ApcBox/mac/build.sh
set -e
cd "$(dirname "$0")"
D=../../lib/DaisySP
mkdir -p build

SRC_DIRS="$D/Source"
DEFS=""
if [ -d "$D/DaisySP-LGPL/Source" ]; then
  SRC_DIRS="$SRC_DIRS $D/DaisySP-LGPL/Source"
  DEFS="-DUSE_DAISYSP_LGPL"
else
  echo "DaisySP-LGPL not found: building without reverb"
fi
INC=$(find $SRC_DIRS -type d | sed 's/^/-I/' | tr '\n' ' ')
SYSINC=$(find $SRC_DIRS -type d | sed 's/^/-isystem /' | tr '\n' ' ')
# Newer Apple clang no longer drags size_t & co. in by accident, and some
# DaisySP headers rely on that. Force the C headers in for everything.
FORCE="-include stddef.h -include stdint.h -include stdlib.h -include string.h -include math.h"
DSP=$(find $SRC_DIRS -name '*.cpp' | tr '\n' ' ')

OS=$(uname)
if [ "$OS" = "Darwin" ]; then
  MIDI_DEF="-D__MACOSX_CORE__"
  LIBS="-framework CoreMIDI -framework CoreAudio -framework AudioToolbox -framework CoreFoundation"
else
  MIDI_DEF=""   # Linux: test build only, RtMidi falls back to no MIDI
  LIBS="-lpthread -ldl -lm"
fi

[ build/miniaudio.o -nt vendor/miniaudio_impl.c ] || cc -O2 -c vendor/miniaudio_impl.c -Ivendor -o build/miniaudio.o
[ build/dsp.a -nt $D/Source/daisysp.h ] || {
  rm -rf build/dsp && mkdir -p build/dsp
  for f in $DSP; do
    c++ -O2 -std=c++17 -w $FORCE $DEFS $INC -c "$f" -o "build/dsp/$(basename "$f" .cpp).o" \
      || echo "skipped $(basename "$f") (not used by ApcBox unless the link below fails)"
  done
  ar rcs build/dsp.a build/dsp/*.o
}
c++ -O2 -std=c++17 -Wall -Wno-unused-parameter $FORCE $DEFS $MIDI_DEF -I.. -isystem vendor $SYSINC \
  sim.cpp ../box.cpp ../engine.cpp vendor/RtMidi.cpp build/miniaudio.o build/dsp.a $LIBS -o build/apcbox
echo "built mac/build/apcbox"
