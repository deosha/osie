# Release builder for Linux archives and packages. Ubuntu 22.04 (glibc 2.35)
# is the oldest baseline: binaries built here run on Debian 12, Ubuntu 22.04+,
# Fedora 36+ and RHEL/Rocky 9+. The C++ runtime is linked statically
# (OSIE_PORTABLE), so only libc/libm are needed at run time.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-12 ninja-build python3-pip rpm file binutils dpkg-dev ca-certificates \
    && pip3 install --no-cache-dir cmake==3.31.6 \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-12 CXX=g++-12
WORKDIR /src
