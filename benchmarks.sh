#!/bin/bash
set -e

###############################################################################
# Paths
###############################################################################

ROOT_DIR="$(pwd)"
BUILD_DIR="$ROOT_DIR/build"
BENCH_DIR="$BUILD_DIR/benchmarks"
SUBLOG="$ROOT_DIR/sub.log"

###############################################################################
# Colors
###############################################################################

GREEN='\033[1;32m'
BLUE='\033[1;34m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
NC='\033[0m'

###############################################################################
# 0. Build Project (Release)
###############################################################################

echo -e "${BLUE}=== Building Project (Release Mode) ===${NC}"

mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR" > /dev/null

cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"

popd > /dev/null


###############################################################################
# 1. Init Core
###############################################################################

echo -e "\n${BLUE}=== Initializing Core ===${NC}"

pushd "$BENCH_DIR" > /dev/null
sudo ./init_bench
popd > /dev/null


###############################################################################
# 2. Helper Function
###############################################################################

run_test() {
    local NAME="$1"
    local TOPIC="$2"
    local TYPE="$3"
    local WRITERS="$4"
    local SIZE="$5"

    echo -e "\n${YELLOW}>>> TEST: ${NAME}  (${TYPE}, Writers: ${WRITERS}, Size: ${SIZE})${NC}"

    # Remove old log
    rm -f "$SUBLOG"

    # Start subscriber in background writing *directly* to ROOT
    pushd "$BENCH_DIR" > /dev/null
    ./bench_sub "$TOPIC" > "$SUBLOG" 2>&1 &
    local SUB_PID=$!
    popd > /dev/null

    sleep 0.25  # give subscriber time to start

    # Publisher(s)
    pushd "$BENCH_DIR" > /dev/null
    if [ "$TYPE" == "SWMR" ]; then
        ./bench_pub_swmr "$TOPIC" "$SIZE"
    else
        ./bench_pub_mwmr "$TOPIC" "$WRITERS" "$SIZE"
    fi
    popd > /dev/null

    sleep 0.15

    kill "$SUB_PID" 2>/dev/null || true

    echo -e "${GREEN}Subscriber Observed Rate:${NC}"

    if grep -q "Rate:" "$SUBLOG"; then
        grep "Rate:" "$SUBLOG" | tail -n 1
    else
        echo -e "${RED}No subscriber data received${NC}"
    fi
}


###############################################################################
# 3. Benchmark Matrix
###############################################################################

run_test "Baseline (Small Ring)"     "small_ring_swmr"   "SWMR" 1 64
run_test "Throughput (Large Ring)"   "large_ring_swmr"   "SWMR" 1 64
run_test "Bandwidth (Huge Msg)"      "huge_msg_swmr"     "SWMR" 1 8192

run_test "MWMR Standard"             "mwmr_std"          "MWMR" 4 64
run_test "MWMR High Contention"      "mwmr_contention"   "MWMR" 4 64


###############################################################################
# 4. Cleanup
###############################################################################

rm -f "$SUBLOG"

echo -e "\n${GREEN}All Benchmarks Completed.${NC}"
