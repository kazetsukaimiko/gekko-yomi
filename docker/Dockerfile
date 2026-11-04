FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake ninja-build pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN cmake -S . -B /tmp/build -G Ninja -DGEKKO_YOMI_BUILD_TESTS=ON \
    && cmake --build /tmp/build \
    && ctest --test-dir /tmp/build --output-on-failure
