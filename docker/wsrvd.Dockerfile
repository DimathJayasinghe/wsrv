#
# Runtime image for the wsrvd sidecar daemon (ADR-0026 amendment 1). Not the dev image (docker/Dockerfile).
#
#   docker build -f docker/wsrvd.Dockerfile -t wsrvd .
#   docker run --rm -p 8080:8080 -v wsrv-ipc:/run/wsrv wsrvd --allow-origin=http://localhost:8000
#
# Why a source build here and not "copy the release binary": one file reproduces the image from a tag on
# any machine, with no download to trust. CI builds it natively per architecture (release.yml).
#
# The daemon runs as uid/gid 10001. Its IPC socket is mode 0600 and the peer uid must equal the daemon's
# uid (ADR-0017), so the container that runs your PHP worker must use the same uid (see compose.yaml).

ARG UBUNTU_VERSION=24.04

FROM ubuntu:${UBUNTU_VERSION} AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-14 gcc-14 cmake ninja-build make pkg-config python3 libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
# The CMake presets call plain `gcc`/`g++`; point them at GCC 14 like the dev image does.
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100
WORKDIR /src
COPY . .
RUN cmake --preset release && cmake --build --preset release -j"$(nproc)" --target wsrvd wsrv_shared \
 && install -d /out \
 && install -m 0755 "$(find build/release -name wsrvd -type f -perm -u+x | head -1)" /out/wsrvd \
 && install -m 0755 "$(find build/release -name 'libwsrv.so.*.*.*' -type f | head -1)" /out/libwsrv.so

FROM ubuntu:${UBUNTU_VERSION} AS runtime
ARG DEBIAN_FRONTEND=noninteractive
ARG VERSION=dev
ARG REVISION=unknown
LABEL org.opencontainers.image.title="wsrvd" \
      org.opencontainers.image.description="WebSocket sidecar daemon (RFC 6455) for PHP and other languages" \
      org.opencontainers.image.source="https://github.com/DimathJayasinghe/wsrv" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}"

# Only the shared libraries the binary links against: no compiler, no shell tools beyond the base image.
RUN apt-get update && apt-get install -y --no-install-recommends libssl3t64 zlib1g libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
 && (userdel -r ubuntu 2>/dev/null || true) \
 && groupadd --gid 10001 wsrv \
 && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin wsrv \
 && install -d -m 0700 -o wsrv -g wsrv /run/wsrv

COPY --from=build /out/wsrvd /usr/local/bin/wsrvd
COPY --from=build /out/libwsrv.so /usr/local/lib/wsrv/libwsrv.so

# A named volume mounted on /run/wsrv copies this directory's owner and mode on first use, so the
# socket directory is private and owned by the daemon user (OPERATIONS section 7).
VOLUME ["/run/wsrv"]
USER 10001:10001
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/wsrvd"]
CMD ["--ipc-socket=/run/wsrv/ipc.sock", "--listen-host=0.0.0.0", "--listen-port=8080"]
