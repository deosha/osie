# Linux build/test environment for OSIE (used for Linux integration tests and
# the isolated CPU-contention experiments; never run privileged).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build libgtest-dev libbenchmark-dev \
      clang clang-tidy libclang-rt-18-dev python3 python3-numpy ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
