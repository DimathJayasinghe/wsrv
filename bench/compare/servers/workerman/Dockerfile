# Workerman 5 on the event extension (libevent): the fastest event loop Workerman offers on Linux
# without Swoole. One process, so a broadcast reaches every connection (see ../../README.md).
FROM php:8.3-cli
# The tarball is fetched directly and pinned: pecl.php.net's index for this package was incomplete when this was written.
ARG EVENT_VERSION=3.1.4
RUN apt-get update && apt-get install -y --no-install-recommends libevent-dev libssl-dev unzip $PHPIZE_DEPS \
    && docker-php-ext-install sockets pcntl \
    && curl -sSL --retry 8 --retry-all-errors -o /tmp/event.tgz "https://pecl.php.net/get/event-$EVENT_VERSION.tgz" \
    && (yes '' | pecl install /tmp/event.tgz) \
    && docker-php-ext-enable --ini-name zz-event.ini event \
    && rm -rf /var/lib/apt/lists/* /tmp/event.tgz
COPY --from=composer:2 /usr/bin/composer /usr/bin/composer
WORKDIR /app
ARG WORKERMAN=^5.0
RUN composer require --no-interaction --no-progress "workerman/workerman:$WORKERMAN"
COPY server.php /app/server.php
# PHP's own memory_limit would stop a server long before the container's memory does; every PHP contender runs with none (the container limit is the same for all).
CMD ["php", "-d", "memory_limit=-1", "/app/server.php", "start"]
