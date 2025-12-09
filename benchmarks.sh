#!/bin/bash
set -euo pipefail

###############################################################################
# USRL BENCHMARK SUITE (v2.0 - Robust)
# Compatible with: Alpine (BusyBox) & Ubuntu (GNU)
# Fixes: Timeout hangs by using explicit background kill
###############################################################################

# --- Config ---
ROOT_DIR="$(pwd)"
BUILD_DIR="$ROOT_DIR/build"
BENCH_DIR="$BUILD_DIR/benchmarks"
SUBLOG="$ROOT_DIR/sub.log"

TCP_SERVER_PORT=8080
TCP_TIMEOUT=30

UDP_SERVER_PORT=9090
UDP_TIMEOUT=28

# --- Colors ---
GREEN='\033[1;32m'
BLUE='\033[1;34m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
NC='\033[0m'

# --- Helper: Robust Timeout ---
# Runs a command and kills it (SIGKILL) if it exceeds duration
# Usage: run_with_timeout 10s ./my_command arg1
run_with_timeout() {
    local dur=$1
    shift
    local cmd="$@"
    
    # Run command in background
    $cmd &
    local cmd_pid=$!
    
    # Watcher: wait duration then force kill
    (sleep "${dur%s}" && kill -9 "$cmd_pid" 2>/dev/null) &
    local watcher_pid=$!
    
    # Wait for command to finish (or be killed)
    wait "$cmd_pid" 2>/dev/null || true
    
    # Kill watcher if command finished early
    kill "$watcher_pid" 2>/dev/null || true
}

# --- Header ---
clear
echo -e "${BLUE}"
cat << "EOF"
  _   _  _____  ____   _      
 | | | |/ ____||  _ \ | |     
 | | | | (___  | |_) || |     
 | |_| |\___ \ |  _ < | |___  
  \___/ ____) || | \ \|_____| 
       |_____/ |_|  \_\       
                              
   Ultra-Low Latency Runtime  
       Benchmark Suite        
EOF
echo -e "${NC}"

###############################################################################
# Cleanup Function
###############################################################################

cleanup() {
    rm -f "$SUBLOG" || true

    # Kill any lingering servers/clients
    pkill -9 -f bench_tcp_server || true
    pkill -9 -f bench_tcp_client || true
    pkill -9 -f bench_tcp_mt || true

    pkill -9 -f bench_udp_server || true
    pkill -9 -f bench_udp_mt || true
    pkill -9 -f bench_udp_flood || true

    sleep 0.1
}
trap cleanup EXIT INT TERM

###############################################################################
# 0. Build Everything
###############################################################################

echo -e "${BLUE}=== 0. Building USRL (Release) ===${NC}"
mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR" > /dev/null
cmake -DCMAKE_BUILD_TYPE=Release .. > /dev/null
make -j"$(nproc)" > /dev/null
popd > /dev/null
echo -e "${GREEN}✓ Build Complete${NC}"

###############################################################################
# 1. Initialize SHM Core
###############################################################################

echo -e "\n${BLUE}=== 1. Initializing SHM Core ===${NC}"

if [ -w /dev/shm ]; then
    rm -f /dev/shm/usrl_core || true
else
    if [ -d /dev/shm ]; then
        sudo rm -f /dev/shm/usrl_core || true
    fi
fi

pushd "$BENCH_DIR" > /dev/null
./init_bench || { echo -e "${RED}init_bench failed!${NC}"; exit 1; }
popd > /dev/null

if [ -f /dev/shm/usrl_core ]; then
    ls -lh /dev/shm/usrl_core
    echo -e "${GREEN}✓ SHM Core Ready${NC}"
else
    echo -e "${YELLOW}⚠ SHM File not found (Mac uses memory object, OK)${NC}"
fi

###############################################################################
# 2. SHM Benchmark Helper
###############################################################################

run_shm_test() {
    local name="$1" topic="$2" type="$3" writers="${4:-1}" size="${5:-64}"
    
    echo -e "\n${YELLOW}>>> SHM: $name (${type}, W:$writers, ${size}B) ${NC}"
    
    rm -f "$SUBLOG"
    
    # Subscriber
    pushd "$BENCH_DIR" > /dev/null
    ./bench_sub "$topic" > "$SUBLOG" 2>&1 &
    local sub_pid=$!
    popd > /dev/null
    
    sleep 0.2
    
    # Publisher(s)
    pushd "$BENCH_DIR" > /dev/null
    if [[ "$type" == "SWMR" ]]; then
        run_with_timeout 25 ./bench_pub_swmr "$topic" "$size"
    else
        run_with_timeout 25 ./bench_pub_mwmr "$topic" "$writers" "$size"
    fi
    popd > /dev/null
    
    sleep 0.2
    kill -9 "$sub_pid" 2>/dev/null || true
    wait "$sub_pid" 2>/dev/null || true
    
    echo -e "${GREEN}✓ Subscriber: $(tail -1 "$SUBLOG" 2>/dev/null || echo "No data")${NC}"
}

###############################################################################
# 3. TCP Benchmark Helper
###############################################################################

run_tcp_test() {
    local name="$1"
    
    echo -e "\n${YELLOW}>>> TCP: $name ${NC}"
    
    pushd "$BENCH_DIR" > /dev/null
    ./bench_tcp_server "$TCP_SERVER_PORT" > /dev/null &
    local server_pid=$!
    echo -e "${BLUE}[Server Started, PID=$server_pid]${NC}"
    popd > /dev/null
    
    # Wait for port
    for i in {1..20}; do
        if nc -z 127.0.0.1 "$TCP_SERVER_PORT" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    
    pushd "$BENCH_DIR" > /dev/null
    run_with_timeout "$TCP_TIMEOUT" ./bench_tcp_client "127.0.0.1" "$TCP_SERVER_PORT"
    popd > /dev/null
    
    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    
    echo -e "${GREEN}✓ TCP Single-Thread Complete${NC}"
}

run_tcp_mt_test() {
    local threads="$1"
    echo -e "\n${YELLOW}>>> TCP: Multi-Threaded ($threads Threads) ${NC}"
    
    pushd "$BENCH_DIR" > /dev/null
    ./bench_tcp_server "$TCP_SERVER_PORT" > /dev/null &
    local server_pid=$!
    echo -e "${BLUE}[Server Started, PID=$server_pid]${NC}"
    popd > /dev/null
    
    for i in {1..20}; do
        if nc -z 127.0.0.1 "$TCP_SERVER_PORT" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    
    pushd "$BENCH_DIR" > /dev/null
    run_with_timeout "$TCP_TIMEOUT" ./bench_tcp_mt "127.0.0.1" "$TCP_SERVER_PORT" "$threads"
    popd > /dev/null
    
    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    
    echo -e "${GREEN}✓ TCP MT ($threads) Complete${NC}"
}

###############################################################################
# 4. UDP Benchmark Helpers (Robust Kill)
###############################################################################

run_udp_test() {
    local name="$1"
    echo -e "\n${YELLOW}>>> UDP: $name ${NC}"

    pushd "$BENCH_DIR" > /dev/null
    ./bench_udp_server "$UDP_SERVER_PORT" > /dev/null &
    local server_pid=$!
    echo -e "${BLUE}[Server Started, PID=$server_pid]${NC}"
    popd > /dev/null

    sleep 0.5 # Wait for bind

    pushd "$BENCH_DIR" > /dev/null
    run_with_timeout "$UDP_TIMEOUT" ./bench_udp_mt "127.0.0.1" "$UDP_SERVER_PORT" 1
    popd > /dev/null

    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    echo -e "${GREEN}✓ UDP Single-Thread Complete${NC}"
}

run_udp_mt_test() {
    local threads="$1"
    echo -e "\n${YELLOW}>>> UDP: Multi-Threaded ($threads Threads) ${NC}"

    pushd "$BENCH_DIR" > /dev/null
    ./bench_udp_server "$UDP_SERVER_PORT" > /dev/null &
    local server_pid=$!
    echo -e "${BLUE}[Server Started, PID=$server_pid]${NC}"
    popd > /dev/null

    sleep 0.5

    pushd "$BENCH_DIR" > /dev/null
    run_with_timeout "$UDP_TIMEOUT" ./bench_udp_mt "127.0.0.1" "$UDP_SERVER_PORT" "$threads"
    popd > /dev/null

    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    echo -e "${GREEN}✓ UDP MT ($threads) Complete${NC}"
}

run_udp_flood_test() {
    echo -e "\n${YELLOW}>>> UDP: Flood Test ${NC}"

    pushd "$BENCH_DIR" > /dev/null
    ./bench_udp_flood "$UDP_SERVER_PORT" > /dev/null &
    local server_pid=$!
    echo -e "${BLUE}[Flood Server Started, PID=$server_pid]${NC}"
    popd > /dev/null

    sleep 0.5

    pushd "$BENCH_DIR" > /dev/null
    run_with_timeout "$UDP_TIMEOUT" ./bench_udp_mt "127.0.0.1" "$UDP_SERVER_PORT" 8
    popd > /dev/null

    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    echo -e "${GREEN}✓ UDP Flood Complete${NC}"
}

###############################################################################
# 5. Master Execution
###############################################################################

echo -e "\n${BLUE}=== SHM BENCHMARKS ===${NC}"
run_shm_test "Small Ring"        "small_ring_swmr"   "SWMR" 1 64
run_shm_test "Large Ring"        "large_ring_swmr"   "SWMR" 1 64  
run_shm_test "Huge Messages"     "huge_msg_swmr"     "SWMR" 1 8192
run_shm_test "MWMR Standard"     "mwmr_std"          "MWMR" 4 64
run_shm_test "MWMR Contention"   "mwmr_contention"   "MWMR" 8 64

echo -e "\n${BLUE}=== TCP BENCHMARKS ===${NC}"
run_tcp_test "Single Thread Request/Response"
run_tcp_mt_test 4
run_tcp_mt_test 8

echo -e "\n${BLUE}=== UDP BENCHMARKS ===${NC}"
run_udp_test "Single Thread Request/Response"
run_udp_mt_test 4
run_udp_mt_test 8
run_udp_flood_test

###############################################################################
# 6. Footer
###############################################################################

echo -e "\n"
echo -e "${GREEN}===========================================${NC}"
echo -e "${GREEN}     BENCHMARK SUITE COMPLETE!             ${NC}"
echo -e "${GREEN}===========================================${NC}"
echo -e "${BLUE} SHM Logs: $SUBLOG${NC}"
echo -e "${BLUE} Tool: USRL Runtime v1.0${NC}"
echo -e "\n"
