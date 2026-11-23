# Stage 1: composer:2 has the zip support that php:cli lacks, so it installs the package.
FROM composer:2 AS vendor
WORKDIR /app
RUN composer require --no-interaction --no-progress "wsrv/client:^0.1@rc"

# Stage 2: a plain PHP CLI image with only the installed vendor/ copied in.
FROM php:8.3-cli
WORKDIR /app
COPY --from=vendor /app/vendor /app/vendor
CMD ["php", "/app/worker.php"]
