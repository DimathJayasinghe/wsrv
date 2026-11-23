# Lesson 42 — A daemon in a container

## Goal
After this lesson you can explain why the `wsrvd` image has two stages, why it runs as a fixed uid, and why two containers can share a Unix socket through a volume.

## The problem
A PHP developer who sees `cmake` closes the tab. A container image removes the build step, and it removes the "glibc 2.39 or newer" floor of the prebuilt files too, because the image brings its own libc. The image has to be small, run as a non-root user, and still let a PHP container reach the daemon's private IPC socket.

## 1. Two stages
`docker/wsrvd.Dockerfile` has a `build` stage (compiler, CMake, headers) and a `runtime` stage that copies out only `wsrvd` and `libwsrv.so` and installs three runtime libraries (`libssl3t64`, `zlib1g`, `libstdc++6`). The compiler never reaches the shipped image: 119 MB, and nothing in it can build anything. The build stage needs `python3` (the repository's own checks run at configure time) and a plain `g++` name (the CMake presets call `g++`; Ubuntu 24.04 installs only `g++-14`, so `update-alternatives` makes it the default, as the dev image does). Both were found by the first two failed builds, not by reading.

## 2. Why a fixed uid
The IPC socket is mode `0600` and the daemon checks the peer uid with `SO_PEERCRED` (ADR-0017). A PHP container running as another uid cannot connect, by design. So the image creates uid 10001 and the compose file runs the worker as `10001:10001` too. Docker copies the owner and mode of an image directory (`/run/wsrv`, `0700`) into a new named volume on first use, so nobody has to `chown` anything.

## 3. Sharing a socket between containers
A Unix socket is a file. Two containers that mount the same volume see the same file, so `connect()` works across them, with the kernel still reporting the real peer uid. No port is opened for IPC; only 8080 (the WebSocket) is published.

## 4. Verified
Image built; `stat` showed `/run/wsrv` `0700` and `ipc.sock` `0600`; curl got `101 Switching Protocols`; a masked text frame sent to port 8080 came back echoed by a PHP worker running in a second container.

## Pitfalls
- Running the worker as root "to make it work" hides the uid rule instead of obeying it.
- `latest` is only for final releases; an `-rc` tag never moves it.
- A new GHCR package is private until its page says otherwise.

## Exercises
1. Remove `user: "10001:10001"` from the worker and read the error. Which line of ADR-0017 explains it?
2. Why is the image built natively on an arm runner instead of with QEMU on one machine?
3. Add `--tls-cert` and `--tls-key` to the compose file: which files must be mounted, with which modes?
