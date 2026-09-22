FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libboost-dev \
        libcurl4-openssl-dev \
        libgflags-dev \
        libgoogle-glog-dev \
        libgrpc++-dev \
        libibverbs-dev \
        libjsoncpp-dev \
        libnuma-dev \
        libprotobuf-dev \
        libssl-dev \
        liburing-dev \
        libyaml-cpp-dev \
        ninja-build \
        pkg-config \
        protobuf-compiler \
        protobuf-compiler-grpc \
        pybind11-dev \
        python3-dev \
    && rm -rf /var/lib/apt/lists/*
