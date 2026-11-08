FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake ninja-build pkg-config ca-certificates libsystemd-dev libspdlog-dev python3 qt6-base-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN cmake -S . -B /tmp/build -G Ninja -DGEKKO_YOMI_BUILD_TESTS=ON \
    && cmake --build /tmp/build \
    && ctest --test-dir /tmp/build --output-on-failure \
    && ! ldd /tmp/build/gekko-yomi-tests | grep -i qt \
    && ldd /tmp/build/gekko-yomi | grep -i qt \
    && cmake -S . -B /tmp/build-off -G Ninja -DGEKKO_YOMI_QT_SHELL=OFF -DGEKKO_YOMI_BUILD_TESTS=OFF \
    && cmake --build /tmp/build-off \
    && ! ldd /tmp/build-off/gekko-yomi | grep -i qt
