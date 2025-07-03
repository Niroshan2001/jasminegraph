#!/bin/bash

if [ "$1" = "--debug" ]; then
    cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_ENABLE_DEBUG=1 .
else
    cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .
fi
cmake --build . --target JasmineGraph -- -j 4

# Ensure debug symbols are not stripped
if [ "$1" = "--debug" ]; then
    echo "Debug build completed with symbols preserved"
    file ./JasmineGraph | grep "not stripped" && echo "Debug symbols confirmed" || echo "Warning: Debug symbols may be missing"
fi
