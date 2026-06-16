#!/bin/bash
# Build the latency probes ON the TX2 board (run from this directory).
# Needs the L4T jetson_multimedia_api Argus headers + libnvargus (both ship with JetPack).
set -e
INC="-I/usr/src/jetson_multimedia_api/argus/include -I/usr/src/jetson_multimedia_api/include"
LIB="-L/usr/lib/aarch64-linux-gnu/tegra -lnvargus -lpthread"
g++ -std=c++11 -O2 argus_evlat.cpp -o argus_evlat $INC $LIB
gcc -O2 v4l2_lat.c -o v4l2_lat -lm
echo "built: ./argus_evlat (Argus SOF->ISP-done, event queue)  ./v4l2_lat (raw V4L2 SOF->app floor)"
