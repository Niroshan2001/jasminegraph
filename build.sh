#!/bin/bash

if [ "$1" = "--debug" ]; then
    echo "Configuring with DEBUG build type..."
    cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_ENABLE_DEBUG=1 .
else
    echo "Configuring with RelWithDebInfo build type..."
    cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .
fi

echo "Starting build process..."
cmake --build . --target JasmineGraph -- -j 4 -v

echo "Build process completed. Checking for binary..."
if [ -f "./JasmineGraph" ]; then
    echo "JasmineGraph binary found!"
    ls -la ./JasmineGraph
else
    echo "JasmineGraph binary NOT found!"
    echo "Checking what was built..."
    ls -la ./*.so ./*.a ./JasmineGraph* 2>/dev/null || echo "No libraries or executables found"
fi

# Ensure debug symbols are not stripped
if [ "$1" = "--debug" ]; then
    echo "Debug build completed with symbols preserved"
    if [ -f "./JasmineGraph" ]; then
        file ./JasmineGraph | grep "not stripped" && echo "Debug symbols confirmed" || echo "Warning: Debug symbols may be missing"
    fi
fi
