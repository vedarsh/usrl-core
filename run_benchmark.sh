#!/bin/bash
set -e

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}[1/5] Building Project...${NC}"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null
make -j4 > /dev/null
cd ..

echo -e "${BLUE}[2/5] Initializing Core...${NC}"
# Ensure config exists
if [ ! -f usrl_config.json ]; then
    echo "Error: usrl_config.json not found!"
    exit 1
fi
./build/init_core

echo -e "${BLUE}[3/5] Running SWMR Benchmark (Single Writer)...${NC}"
# Start subscriber in background, saving output to log
./build/bench_sub bench_swmr > swmr_sub.log &
SUB_PID=$!

# Give subscriber a moment to warm up
sleep 0.5

# Run publisher
./build/bench_pub_swmr bench_swmr

# Stop subscriber
kill $SUB_PID 2>/dev/null || true

# Show subscriber stats
echo -e "${GREEN}Subscriber Stats (Peak):${NC}"
grep "Rate:" swmr_sub.log | tail -n 3

echo -e "${BLUE}[4/5] Running MWMR Benchmark (4 Concurrent Writers)...${NC}"
# Start subscriber
./build/bench_sub bench_mwmr > mwmr_sub.log &
SUB_PID=$!

sleep 0.5

# Run multi-publisher
./build/bench_pub_mwmr bench_mwmr

kill $SUB_PID 2>/dev/null || true

echo -e "${GREEN}Subscriber Stats (Peak):${NC}"
grep "Rate:" mwmr_sub.log | tail -n 3

echo -e "${BLUE}[5/5] Cleanup...${NC}"
rm swmr_sub.log mwmr_sub.log
echo -e "${GREEN}Done!${NC}"
