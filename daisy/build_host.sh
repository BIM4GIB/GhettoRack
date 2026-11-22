#!/bin/sh
# Builds and runs the desktop engine test
cd "$(dirname "$0")"
D=../lib/DaisySP
mkdir -p hostbuild
SRCS=$(find $D/Source $D/DaisySP-LGPL/Source -name '*.cpp')
INC=$(find $D/Source $D/DaisySP-LGPL/Source -type d | sed 's/^/-I/' | tr '\n' ' ')
g++ -O2 -std=c++17 -DUSE_DAISYSP_LGPL -DAB_HOST_DEBUG -Wall $INC engine.cpp host_test.cpp $SRCS -o hostbuild/host_test 2>&1 | grep -v "DaisySP/Source\|DaisySP-LGPL" | grep -A3 "engine\|host_test"
cd hostbuild && ./host_test
# The Daisy's control side behind a pty, for t95/bridge/test_bridge.py (EndToEndTest)
cd .. && g++ -O1 -std=c++17 -DUSE_DAISYSP_LGPL -w $INC engine.cpp t95sim.cpp $SRCS -o hostbuild/t95sim -lutil && echo "built hostbuild/t95sim"
