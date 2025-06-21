#!/bin/bash

if [ "$1" = "--debug" ]; then
    cmake -DCMAKE_BUILD_TYPE=Debug clean .
else
    cmake -DCMAKE_BUILD_TYPE=Release clean .
fi
cmake --build . --target JasmineGraph -- -j 4
