FROM miyurud/jasminegraph-prerequisites:20260321T165531

RUN apt-get update && apt-get install -y libcurl4-openssl-dev sysstat nmon python3-setuptools
RUN rm -r /usr/lib/python3.8/distutils
RUN apt-get purge -y libpython3.8-dev python3.8-dev python3.8-distutils libpython3.8 python3.8

ENV HOME="/home/ubuntu"
ENV JASMINEGRAPH_HOME="${HOME}/software/jasminegraph"

RUN ln -sf /usr/bin/python3.8 /usr/bin/python3

WORKDIR "${JASMINEGRAPH_HOME}"

# Install build dependencies and prometheus-cpp (for histogram metrics)
RUN apt-get update && apt-get install -y --no-install-recommends \
	git \
	cmake \
	build-essential \
	pkg-config \
	libprotobuf-dev \
	libcurl4-openssl-dev \
	libssl-dev \
	ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Optional: build and install prometheus-cpp here if you want native prometheus-cpp integration.
# Skipping building prometheus-cpp inside the image by default because some environments
# have missing third-party build dependencies (civetweb etc). The code already contains
# a fallback that pushes histogram bucket/count/sum lines via Pushgateway, which Prometheus
# can use for histogram_quantile() in Grafana. To enable prometheus-cpp, either:
#  - Install prometheus-cpp on the base image or host, or
#  - Re-enable the build steps below and ensure required dev packages are present.
## RUN git clone --depth 1 --recurse-submodules https://github.com/jupp0r/prometheus-cpp.git /tmp/prometheus-cpp \
##  && mkdir -p /tmp/prometheus-cpp/build \
##  && cd /tmp/prometheus-cpp/build \
##  && cmake .. -DBUILD_SHARED_LIBS=ON -DENABLE_PUSH=ON -DENABLE_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
##  && make -j$(nproc) && make install \
##  && rm -rf /tmp/prometheus-cpp

ARG DEBUG="false"
RUN if [ "$DEBUG" = "true" ]; then apt-get update \
&& apt-get install --no-install-recommends -y gdb gdbserver \
&& apt-get clean; fi

WORKDIR "${JASMINEGRAPH_HOME}"
COPY ./build.sh ./build.sh
COPY ./CMakeLists.txt ./CMakeLists.txt
COPY ./main.h ./main.h
COPY ./main.cpp ./main.cpp
COPY ./globals.h ./globals.h
COPY ./src ./src
COPY ./globals.h ./src/globals.h

RUN if [ "$DEBUG" = "true" ]; then echo "building in DEBUG mode" && sh build.sh --debug; else sh build.sh; fi

COPY ./run-docker.sh ./run-docker.sh
COPY ./src_python ./src_python
COPY ./conf ./conf
COPY ./k8s ./k8s
COPY ./ddl ./ddl

ENTRYPOINT ["/home/ubuntu/software/jasminegraph/run-docker.sh"]
CMD ["bash"]
