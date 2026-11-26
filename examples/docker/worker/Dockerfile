# Stage 1: composer:2 has the zip support that php:cli lacks, so it installs the package.
FROM composer:2 AS vendor
WORKDIR /app
# The package constraint; a release candidate needs its exact version, a final release can use ^1.0
# (WSRV_CLIENT=^1.0 docker compose build).
ARG WSRV_CLIENT=1.0.0-rc.1
RUN composer require --no-interaction --no-progress "wsrv/client:${WSRV_CLIENT}"

# Stage 2: a plain PHP CLI image with only the installed vendor/ copied in.
FROM php:8.3-cli
WORKDIR /app
COPY --from=vendor /app/vendor /app/vendor
CMD ["php", "/app/worker.php"]
