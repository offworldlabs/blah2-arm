FROM debian:bookworm as blah2_env
LABEL maintainer="Jehan <jehan.azad@gmail.com>"
LABEL org.opencontainers.image.source https://github.com/30hours/blah2

ARG OWL_GPU_FIR=OFF

WORKDIR /opt/blah2
ADD lib lib

# Install base dependencies (note: libfftw3-dev removed - building from source with NEON)
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    cmake \
    ninja-build \
    git \
    curl \
    zip \
    unzip \
    doxygen \
    graphviz \
    expect \
    pkg-config \
    gfortran \
    libhackrf-dev \
    libusb-dev \
    libusb-1.0.0-dev \
    libiio-dev \
    python3-pip \
    python3-mako \
    python3-numpy \
    libboost-all-dev \
    && apt-get autoremove -y \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/*

# The default image remains CPU-only. An explicit GPU build supplies the
# selected Vulkan toolchain and a detached, clean VkFFT source checkout; it
# never copies a host driver into the image.
RUN if [ "$OWL_GPU_FIR" = "ON" ]; then \
      apt-get update && apt-get install -y --no-install-recommends \
        libvulkan-dev glslang-dev glslang-tools \
      && rm -rf /var/lib/apt/lists/* \
      && git clone https://github.com/DTolm/VkFFT.git /opt/VkFFT \
      && git -C /opt/VkFFT checkout --detach 066a17c17068c0f11c9298d848c2976c71fad1c1 \
      && test "$(git -C /opt/VkFFT rev-parse HEAD)" = "066a17c17068c0f11c9298d848c2976c71fad1c1" \
      && test -z "$(git -C /opt/VkFFT status --porcelain)"; \
    fi

# Build FFTW from source with ARM NEON optimizations (2-4x faster FFTs)
# Building both double and single precision for flexibility
# Uses generic ARMv8+SIMD target for portability across all 64-bit Pi models (3/4/5)
RUN cd /tmp \
    && curl -L http://www.fftw.org/fftw-3.3.10.tar.gz -o fftw-3.3.10.tar.gz \
    && tar xzf fftw-3.3.10.tar.gz \
    && cd fftw-3.3.10 \
    # Build double-precision with NEON (what blah2 uses)
    && ./configure --enable-shared --enable-threads --enable-neon --prefix=/usr/local \
        CFLAGS="-O3 -march=armv8-a+simd" \
    && make -j$(nproc) \
    && make install \
    # Build single-precision with NEON (for safety/future use)
    && make clean \
    && ./configure --enable-shared --enable-threads --enable-neon --enable-single --prefix=/usr/local \
        CFLAGS="-O3 -march=armv8-a+simd" \
    && make -j$(nproc) \
    && make install \
    && ldconfig \
    && cd /tmp \
    && rm -rf fftw-3.3.10*

# install dependencies from vcpkg
ENV VCPKG_ROOT=/opt/vcpkg
ENV CC=/usr/bin/gcc
ENV CXX=/usr/bin/g++
RUN export PATH="/opt/vcpkg:${PATH}" \
    && git clone --branch 2026.06.24 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && if [ "$(uname -m)" = "aarch64" ]; then export VCPKG_FORCE_SYSTEM_BINARIES=1; fi \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && cd /opt/blah2/lib && vcpkg integrate install \
    && vcpkg install --clean-after-build

# install SDRplay API
USER root

RUN export ARCH=$(uname -m) \
    && if [ "$ARCH" = "x86_64" ]; then \
         ARCH="amd64"; \
    elif [ "$ARCH" = "aarch64" ]; then \
         ARCH="arm64"; \
    fi \
    && export MAJVER="3.15" \
    && export MINVER="2" \
    && export VER=${MAJVER}.${MINVER} \
    && cd /opt/blah2/lib/sdrplay-${VER} \
    && chmod +x SDRplay_RSP_API-Linux-${VER}.run \
    && ./SDRplay_RSP_API-Linux-${VER}.run --noexec --target /opt/blah2/lib/sdrplay-${VER}/extract \


    # Then manually copy the files to the target location
    && cp -r /opt/blah2/lib/sdrplay-${VER}/extract/* /opt/blah2/lib/sdrplay-${VER}/ \

    && cp ${ARCH}/libsdrplay_api.so.${MAJVER} /usr/local/lib/libsdrplay_api.so.${MAJVER} \
    && cp inc/* /usr/local/include \
    && chmod 644 /usr/local/lib/libsdrplay_api.so.${MAJVER} \
    && ldconfig


FROM blah2_env as blah2
LABEL maintainer="Jehan <jehan.azad@gmail.com>"

ARG OWL_GPU_FIR=OFF

WORKDIR /opt/blah2

ADD src src
ADD test test
ADD script script
ADD CMakeLists.txt CMakePresets.json Doxyfile ./

# Updated build step to use the correct binary location
RUN set -ex \
    && mkdir -p build \
    && cd build \
    && GPU_CMAKE_ARGS="" \
    && if [ "$OWL_GPU_FIR" = "ON" ]; then GPU_CMAKE_ARGS="-DOWL_GPU_FIR=ON -DOWL_VKFFT_ROOT=/opt/VkFFT"; fi \
    && cmake -S .. --preset prod-release ${GPU_CMAKE_ARGS} \
        -DCMAKE_PREFIX_PATH=$(echo /opt/blah2/lib/vcpkg_installed/*/share) \
    && cd prod-release \
    && make \
    && echo "==== Binary location: ====" \
    && ls -l /opt/blah2/bin/blah2 \
    && mkdir -p /blah2/bin \
    && cp -v /opt/blah2/bin/blah2 /blah2/bin/ \
    && chmod +x /blah2/bin/blah2

# Run the unit tests here, in the build stage, because this is the only image
# that has them: the runtime stage deliberately ships the binary alone. The
# workflow used to run them against the runtime image, where the directory does
# not exist, and swallowed that with `exit 0`, so no test had ever actually
# executed in CI.
#
# set -e means a failing test fails the build. The emptiness checks mean a
# missing or empty test directory fails the build too, rather than passing
# silently the way the old step did.
RUN set -eu; \
    test -d /opt/blah2/bin/test/unit; \
    test -n "$(find /opt/blah2/bin/test/unit -maxdepth 1 -type f -perm -111 -print -quit)"; \
    export BLAH2_FFT_CACHE=/tmp/blah2-fft-length.cache; \
    ctest --test-dir /opt/blah2/build/prod-release --output-on-failure

WORKDIR /blah2/bin

# =============================================================================
# Stage 3: Runtime (DEFAULT - this is what gets deployed)
# Minimal image with only the binary and runtime libraries
# For full build environment: docker build --target blah2 -t blah2:dev .
# =============================================================================
FROM debian:bookworm-slim as runtime
LABEL maintainer="Jehan <jehan.azad@gmail.com>"
LABEL org.opencontainers.image.source https://github.com/30hours/blah2

ARG OWL_GPU_FIR=OFF

# Install only runtime dependencies (no dev packages, no compilers)
# Note: libfftw3-double3 removed - using NEON-optimized build from source
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgfortran5 \
    procps \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# GPU builds own their complete V3D userspace driver. The host supplies only
# the DRM kernel device; never bind host Mesa libraries into this image.
# Pin the Bookworm backport; validate each packaged version on the target GPU.
ARG OWL_MESA_VERSION=25.0.7-2~bpo12+1
RUN if [ "$OWL_GPU_FIR" = "ON" ]; then \
      test "$(dpkg --print-architecture)" = arm64 \
      && printf '%s\n' 'deb http://deb.debian.org/debian bookworm-backports main' \
        > /etc/apt/sources.list.d/owl-gpu-backports.list \
      && apt-get update && apt-get install -y --no-install-recommends \
        libvulkan1 glslang-dev \
      && apt-get install -y --no-install-recommends -t bookworm-backports \
        "mesa-vulkan-drivers=${OWL_MESA_VERSION}" \
      && test -f /usr/share/vulkan/icd.d/broadcom_icd.json \
      && test -f /usr/lib/aarch64-linux-gnu/libvulkan_broadcom.so \
      && ldd /usr/lib/aarch64-linux-gnu/libvulkan_broadcom.so > /tmp/owl-v3d-ldd \
      && ! grep -q 'not found' /tmp/owl-v3d-ldd \
      && rm /tmp/owl-v3d-ldd \
      && rm -rf /var/lib/apt/lists/*; \
    fi

# Copy NEON-optimized FFTW libraries from build stage
COPY --from=blah2_env /usr/local/lib/libfftw3.so* /usr/local/lib/
COPY --from=blah2_env /usr/local/lib/libfftw3f.so* /usr/local/lib/
COPY --from=blah2_env /usr/local/lib/libfftw3_threads.so* /usr/local/lib/
COPY --from=blah2_env /usr/local/lib/libfftw3f_threads.so* /usr/local/lib/

# Copy OpenBLAS libraries from vcpkg build stage
COPY --from=blah2_env /opt/blah2/lib/vcpkg_installed/*/lib/libopenblas* /usr/local/lib/

RUN ldconfig

# Copy the compiled binary from build stage
COPY --from=blah2 /blah2/bin/blah2 /opt/blah2/bin/blah2

# Copy SDRplay restart script (requires pid:host at runtime)
COPY sdrplay-restart.sh /opt/blah2/sdrplay-restart.sh
RUN chmod +x /opt/blah2/sdrplay-restart.sh

WORKDIR /opt/blah2/bin
