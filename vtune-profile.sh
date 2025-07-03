#!/bin/bash

# VTune profiling script for JasmineGraph with improved symbol resolution
# This script ensures VTune can resolve function names properly

CONTAINER_NAME="jasminegraph"
BINARY_PATH="/home/ubuntu/software/jasminegraph/JasmineGraph"
RESULT_DIR="vtune_results_$(date +%Y%m%d_%H%M%S)"
VTUNE_PATH="/opt/intel/oneapi/vtune/2025.3/bin64/vtune"

echo "Starting VTune profiling for JasmineGraph with symbol resolution..."

# Step 1: Find the container ID
echo "1. Finding JasmineGraph container..."
CONTAINER_ID=$(docker ps --format "table {{.ID}}\t{{.Image}}\t{{.Names}}" | grep jasminegraph | head -1 | awk '{print $1}')

if [ -z "$CONTAINER_ID" ]; then
    echo "Error: JasmineGraph container not found!"
    echo "Available containers:"
    docker ps
    exit 1
fi

echo "Found container: $CONTAINER_ID"

# Step 2: Get the container's main process host PID
echo "2. Getting container's main process host PID..."
HOST_PID=$(docker inspect --format '{{.State.Pid}}' $CONTAINER_ID)

if [ -z "$HOST_PID" ]; then
    echo "Error: Could not get host PID for container $CONTAINER_ID"
    exit 1
fi

echo "Container host PID: $HOST_PID"

# Step 3: Find the JasmineGraph process PID in the container
echo "3. Finding JasmineGraph process PID in container..."
echo "Processes in container:"
sudo nsenter -t $HOST_PID -p ps aux | grep JasmineGraph | head -5

JGRAPH_PID=$(sudo nsenter -t $HOST_PID -p ps aux | grep './JasmineGraph' | grep -v grep | awk '{print $2}' | head -1)

if [ -z "$JGRAPH_PID" ]; then
    echo "Error: JasmineGraph process not found in container!"
    echo "Make sure JasmineGraph is running in the container."
    exit 1
fi

echo "JasmineGraph process PID: $JGRAPH_PID"

# Step 4: Set up comprehensive symbol directories
echo "4. Setting up symbol directories..."

# Create local symbol directory
mkdir -p ./symbols/jasminegraph
mkdir -p ./symbols/libs

# Copy the binary with debug symbols
echo "Copying JasmineGraph binary..."
docker cp $CONTAINER_ID:$BINARY_PATH ./symbols/jasminegraph/JasmineGraph

# Copy essential libraries that might be needed
echo "Copying essential libraries..."
docker cp $CONTAINER_ID:/lib/x86_64-linux-gnu/libc.so.6 ./symbols/libs/ 2>/dev/null || true
docker cp $CONTAINER_ID:/usr/lib/x86_64-linux-gnu/libstdc++.so.6 ./symbols/libs/ 2>/dev/null || true

# Verify the binary has debug symbols
echo "Verifying binary has debug symbols..."
if file ./symbols/jasminegraph/JasmineGraph | grep -q "not stripped"; then
    echo "✓ Binary contains debug symbols"
else
    echo "⚠ Warning: Binary may not contain debug symbols"
fi

# Step 5: Run VTune with comprehensive symbol search paths
echo "5. Starting VTune profiler..."
echo "VTune will run for 30 seconds. During this time, run your JasmineGraph workload!"
echo "Press Ctrl+C to stop profiling early if needed."
echo ""

# Use multiple symbol search directories and finalization options
sudo $VTUNE_PATH -collect hotspots \
    -search-dir sym:rp=./symbols/jasminegraph \
    -search-dir sym:rp=./symbols/libs \
    -search-dir sym:rp=/home/ubuntu/software/jasminegraph \
    -search-dir sym:rp=/lib/x86_64-linux-gnu \
    -search-dir sym:rp=/usr/lib/x86_64-linux-gnu \
    -finalization-mode=full \
    -target-pid $JGRAPH_PID \
    -result-dir $RESULT_DIR \
    -duration 30

if [ $? -eq 0 ]; then
    echo ""
    echo "VTune profiling completed successfully!"
    echo "Results saved in: $RESULT_DIR"
    echo ""
    echo "To view results with proper symbols:"
    echo "1. GUI: vtune-gui $RESULT_DIR"
    echo "2. Command line: $VTUNE_PATH -report hotspots -result-dir $RESULT_DIR"
    echo ""
    echo "If you still see hex addresses, try manually setting symbol paths in VTune GUI:"
    echo "   Analysis -> Symbol Search Paths -> Add:"
    echo "   - $(pwd)/symbols/jasminegraph"
    echo "   - /home/ubuntu/software/jasminegraph"
else
    echo ""
    echo "VTune profiling failed. Check the error messages above."
    exit 1
fi
