# Build environment for vgpu-accel: cross-compiles vgpu.sys and the Mesa virgl ICD on Linux.
#
#   ./fetch-ewdk.sh                       # once: EWDK subset into ./ewdk (ISO cached outside Docker)
#   docker build -t vgpu-accel-env .
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" vgpu-accel-env ./build.sh
#
# The image contains parts of Microsoft's EWDK, which may not be redistributed: build it locally
# and do not publish it. It contains no signing key; build.sh creates one in certs/ on first use.
FROM docker.io/library/debian:bookworm@sha256:2c037a04925515fdd6ea85ea14a682d0e79931f5e9f5d07b6dbfc6ba12f9e858

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git curl openssl python3 python3-venv \
        clang lld llvm \
        gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix \
        gcc-mingw-w64-i686-posix g++-mingw-w64-i686-posix \
        ninja-build bison flex glslang-tools pkg-config osslsigncode \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/lib/llvm-14/bin/clang /usr/local/bin/clang-cl \
    && ln -s /usr/lib/llvm-14/bin/lld-link /usr/local/bin/lld-link \
    && update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix \
    && update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix \
    && update-alternatives --set i686-w64-mingw32-gcc /usr/bin/i686-w64-mingw32-gcc-posix \
    && update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-posix

# Meson and Mesa's Python build dependencies
RUN python3 -m venv /opt/mesa-venv \
    && /opt/mesa-venv/bin/pip install --no-cache-dir meson==1.7.2 mako==1.3.10 PyYAML==6.0.2 packaging==25.0
ENV PATH=/opt/mesa-venv/bin:$PATH

# makecat creates the driver catalog (.cat) on Linux; built with a pinned Rust, then Rust is removed
ARG RUST_VERSION=1.89.0
ARG MAKECAT_COMMIT=ee7aa4d
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain "$RUST_VERSION" \
    && git clone https://github.com/anonymix007/makecat.git /tmp/makecat \
    && git -C /tmp/makecat checkout -q "$MAKECAT_COMMIT" \
    && /root/.cargo/bin/cargo install --locked --path /tmp/makecat --root /usr/local \
    && rm -rf /tmp/makecat /root/.cargo /root/.rustup

# EWDK subset from ./fetch-ewdk.sh; the clang VFS overlay is regenerated for its path in the image
COPY ewdk/ /opt/ewdk/
RUN test -f "/opt/ewdk/Program Files/Windows Kits/10/Include/wdf/kmdf/1.27/wdf.h" \
    && python3 /opt/ewdk/generate_vfs.py \
        --msvc "/opt/ewdk/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC" \
        --sdk "/opt/ewdk/Program Files/Windows Kits/10" \
        --out /opt/ewdk/vfs-overlay.json

ENV HOME=/tmp
WORKDIR /src
