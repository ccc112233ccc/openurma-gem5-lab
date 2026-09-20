FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake bc binutils-aarch64-linux-gnu bison build-essential \
    busybox-static bzip2 ca-certificates cmake cpio curl device-tree-compiler \
    flex g++-aarch64-linux-gnu gcc-aarch64-linux-gnu git graphviz \
    libboost-all-dev libelf-dev libgoogle-perftools-dev libhdf5-dev \
    libibverbs-dev libncurses-dev libnl-3-dev libnl-genl-3-dev \
    libnl-route-3-dev libnuma-dev libpng-dev libprotobuf-dev libssl-dev libsystemc-dev \
    meson ninja-build pkg-config protobuf-compiler python3 python3-dev \
    python3-pip python3-pydot python3-setuptools python3-six python3-yaml \
    rsync scons unzip xz-utils zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/openurma-gem5-lab
CMD ["sleep", "infinity"]
