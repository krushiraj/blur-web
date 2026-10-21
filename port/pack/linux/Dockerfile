FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    clang lld make pkg-config python3 ca-certificates file xz-utils \
    libunicorn-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
    libspirv-cross-c-shared-dev libspirv-cross-c-shared0 \
    git cmake libvulkan-dev spirv-headers patchelf zip curl libjson-perl autoconf automake libtool flex bison libx11-dev \
 && rm -rf /var/lib/apt/lists/*
# the lifted code needs a recent clang (musttail + codegen close to the Apple clang the mac build uses); Debian's clang 14 is too old
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gnupg wget \
 && wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-17 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends clang-17 lld-17 \
 && ln -sf /usr/bin/clang-17 /usr/local/bin/clang && ln -sf /usr/bin/ld.lld-17 /usr/local/bin/ld.lld \
 && rm -rf /var/lib/apt/lists/*
