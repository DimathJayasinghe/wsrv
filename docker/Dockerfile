#
# Multi-stage image for wsrv.
#   dev      : full toolchain + sanitizers/fuzzing/analysis tools, passwordless sudo, host-UID user.
#              DEVELOPMENT ONLY. Never ship this stage.
#   (runtime stage is added at milestone M6 when a server binary exists.)
#
# Build:  docker build -f docker/Dockerfile --target dev -t wsrv-dev .
# Base is pinned by tag now; pin by digest in CI (see docs/BUILD_AND_TOOLING.md).

ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION} AS dev

ARG DEBIAN_FRONTEND=noninteractive
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

# Toolchain: GCC 14 + Clang 18 (project minimums: GCC 13 / Clang 17).
# Optional-feature libs: OpenSSL (TLS), zlib (deflate). Binding work: PHP CLI + FFI (M12).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-14 gcc-14 \
        clang-18 clang-format-18 clang-tidy-18 clang-tools-18 libc++-18-dev libc++abi-18-dev \
        lld-18 llvm-18 libclang-rt-18-dev \
        cmake ninja-build ccache pkg-config make \
        gdb valgrind strace ltrace linux-tools-common linux-tools-generic \
        cppcheck lcov gcovr iwyu \
        libssl-dev zlib1g-dev \
        git openssh-client ca-certificates curl wget jq \
        python3 python3-venv python3-pip \
        php-cli php-ffi php-mbstring php-curl \
        sudo less vim nano bash-completion \
    && rm -rf /var/lib/apt/lists/*

# Make the newest toolchain the default and give unversioned names to the tools we call in scripts.
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100 \
 && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100 \
 && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100 \
 && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100 \
 && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-18 100 \
 && update-alternatives --install /usr/bin/llvm-cov llvm-cov /usr/bin/llvm-cov-18 100 \
 && update-alternatives --install /usr/bin/llvm-profdata llvm-profdata /usr/bin/llvm-profdata-18 100 \
 && update-alternatives --install /usr/bin/llvm-symbolizer llvm-symbolizer /usr/bin/llvm-symbolizer-18 100

# Non-root dev user that matches the host UID/GID so files created in the bind mount are owned by you,
# with passwordless sudo for installing extra packages inside the container (dev image only).
# Ubuntu 24.04 ships a default 'ubuntu' user at UID 1000; remove it to avoid clashes.
RUN (userdel -r ubuntu 2>/dev/null || true) \
 && (getent group ${USER_GID} >/dev/null || groupadd --gid ${USER_GID} ${USERNAME}) \
 && useradd --uid ${USER_UID} --gid ${USER_GID} --create-home --shell /bin/bash ${USERNAME} \
 && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
 && chmod 0440 /etc/sudoers.d/${USERNAME}

# Pre-create the ccache directory owned by the dev user. A named volume mounted over a path that does
# not exist in the image is created root-owned, which makes ccache fail with 'Permission denied'.
RUN mkdir -p /home/${USERNAME}/.ccache && chown -R ${USER_UID}:${USER_GID} /home/${USERNAME}/.ccache

# Sanitizer/ccache ergonomics.
ENV CCACHE_DIR=/home/${USERNAME}/.ccache \
    CCACHE_MAXSIZE=5G \
    CMAKE_CXX_COMPILER_LAUNCHER=ccache \
    ASAN_OPTIONS=strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1:detect_leaks=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    LANG=C.UTF-8

USER ${USERNAME}
# No fixed WORKDIR: docker-compose mounts the repo at its real host path and sets working_dir to it.
CMD ["bash"]
