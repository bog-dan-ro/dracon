# Debian trixie is the oldest distribution Dracon targets: gcc 14, cmake 3.31,
# boost 1.83. Build and test the server in it with:
#     docker build -t dracon .
FROM debian:trixie-slim

RUN apt-get update && apt-get -y install --no-install-recommends \
  ca-certificates \
  cmake \
  g++ \
  libboost-coroutine-dev \
  libboost-iostreams-dev \
  libboost-log-dev \
  libboost-program-options-dev \
  libboost-test-dev \
  libcurl4-openssl-dev \
  libssl-dev \
  ninja-build \
  && rm -rf /var/lib/apt/lists/*

# The sources come last so that a code change doesn't invalidate the apt layer
WORKDIR /src
COPY . /src

RUN cmake -G Ninja -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build \
 && ctest --test-dir /build --output-on-failure
