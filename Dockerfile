FROM miyurud/jasminegraph-prerequisites:20241231T070657

RUN apt-get update && apt-get install -y libcurl4-openssl-dev sysstat nmon
RUN rm -r /usr/lib/python3.8/distutils
RUN apt-get purge -y libpython3.8-dev python3.8-dev python3.8-distutils libpython3.8 python3.8

ENV HOME="/home/ubuntu"
ENV JASMINEGRAPH_HOME="${HOME}/software/jasminegraph"

RUN ln -sf /usr/bin/python3.8 /usr/bin/python3

WORKDIR "${JASMINEGRAPH_HOME}"

ARG DEBUG="false"
RUN if [ "$DEBUG" = "true" ]; then apt-get update \
&& apt-get install --no-install-recommends -y gdb gdbserver binutils \
&& apt-get clean; fi

# Install debug symbol packages for better VTune support
RUN apt-get update && apt-get install -y \
    libc6-dbg \
    binutils \
    file \
    strace \
    && apt-get clean


WORKDIR "${JASMINEGRAPH_HOME}"
COPY ./build.sh ./build.sh
COPY ./CMakeLists.txt ./CMakeLists.txt
COPY ./main.h ./main.h
COPY ./main.cpp ./main.cpp
COPY ./globals.h ./globals.h
COPY ./src ./src

RUN if [ "$DEBUG" = "true" ]; then echo "building in DEBUG mode" && sh build.sh --debug 2>&1 | tee build.log; else sh build.sh 2>&1 | tee build.log; fi || echo "Build failed, continuing..."

# Show build log on failure
RUN if [ ! -f "/home/ubuntu/software/jasminegraph/JasmineGraph" ]; then \
    echo "=== BUILD FAILED - Showing build log ===" && \
    cat build.log && \
    echo "=== END BUILD LOG ==="; fi

# Verify the build was successful and show file information
RUN ls -la /home/ubuntu/software/jasminegraph/
RUN find /home/ubuntu/software/jasminegraph/ -name "JasmineGraph*" -type f
RUN if [ -f "/home/ubuntu/software/jasminegraph/JasmineGraph" ]; then \
    echo "JasmineGraph binary found!" && \
    file /home/ubuntu/software/jasminegraph/JasmineGraph && \
    ldd /home/ubuntu/software/jasminegraph/JasmineGraph; \
    else echo "ERROR: JasmineGraph binary not found!"; fi

# Debug: Show CMake and build files
RUN ls -la /home/ubuntu/software/jasminegraph/CMakeFiles/ || echo "No CMakeFiles directory"
RUN ls -la /home/ubuntu/software/antlr/ || echo "No antlr directory found"

COPY ./run-docker.sh ./run-docker.sh
COPY ./src_python ./src_python
COPY ./conf ./conf
COPY ./k8s ./k8s
COPY ./ddl ./ddl

ENTRYPOINT ["/home/ubuntu/software/jasminegraph/run-docker.sh"]
CMD ["bash"]
