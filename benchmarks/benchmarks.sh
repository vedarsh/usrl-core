#!/bin/bash
set -e

# Setup Paths
BUILD_DIR="../build"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

# 0. Build Project
echo -e "${BLUE}=== Building Project (Release Mode) ===${NC}"
mkdir -p $BUILD_DIR
pushd $BUILD_DIR > /dev/null
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
popd > /dev/null

# 1. Init Core
echo -e "\n${BLUE}=== Initializing Core ===${NC}"
$BUILD_DIR/init_bench

# 2. Helper Function
run_test() {
    NAME=$1
    TOPIC=$2
    TYPE=$3
    WRITERS=$4
    SIZE=$5

    echo -e "\n${BLUE}>>> TEST: $NAME ($TYPE, Writers: $WRITERS, Size: $SIZE)${NC}"

    # Start Subscriber in background
    # Redirect both stdout (1) and stderr (2) to log file
    $BUILD_DIR/bench_sub $TOPIC > sub.log 2>&1 &
    SUB_PID=$!

    # Give sub time to start
    sleep 0.2

    # Start Publisher(s)
    if [ "$TYPE" == "SWMR" ]; then
        $BUILD_DIR/bench_pub_swmr $TOPIC $SIZE
    else
        $BUILD_DIR/bench_pub_mwmr $TOPIC $WRITERS $SIZE
    fi

    # Wait a tiny bit for sub to flush final stats
    sleep 0.1

    # Stop Subscriber
    kill $SUB_PID 2>/dev/null || true

    # Print Result from log
    echo -e "${GREEN}Subscriber Observed Rate:${NC}"
    grep "Rate:" sub.log | tail -n 1 || echo "No data received"
}

# 3. Execute Matrix
run_test "Baseline (Small Ring)" "small_ring_swmr" "SWMR" 1       64
run_test "Throughput (Large Ring)" "large_ring_swmr" "SWMR" 1     64
run_test "Bandwidth (Huge Msg)"  "huge_msg_swmr"   "SWMR" 1       8192

run_test "MWMR Standard"         "mwmr_std"        "MWMR" 4       64
run_test "MWMR High Contention"  "mwmr_contention" "MWMR" 4       64

# Cleanup
rm -f sub.log
echo -e "\n${GREEN}All Benchmarks Completed.${NC}"
