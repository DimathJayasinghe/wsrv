# Laravel Reverb (the Pusher-protocol server of the Laravel ecosystem) on the ev extension.
# A fresh Laravel app with Reverb installed and nothing else, started with `reverb:start`.
FROM php:8.3-cli
ARG EV_VERSION=1.2.0
RUN apt-get update && apt-get install -y --no-install-recommends git unzip libzip-dev $PHPIZE_DEPS \
    && docker-php-ext-install pcntl zip \
    && curl -sSL --retry 8 --retry-all-errors -o /tmp/ev.tgz "https://pecl.php.net/get/ev-$EV_VERSION.tgz" \
    && (yes '' | pecl install /tmp/ev.tgz) \
    && docker-php-ext-enable ev \
    && rm -rf /var/lib/apt/lists/* /tmp/ev.tgz
COPY --from=composer:2 /usr/bin/composer /usr/bin/composer
WORKDIR /app
RUN composer create-project --no-interaction --no-progress laravel/laravel . \
    && composer require --no-interaction --no-progress laravel/reverb
# `reverb:install` asks questions and cannot run without a terminal; the package's own config
# (read from the REVERB_* variables below) is all `reverb:start` needs.
# The app settings the load generator uses (bench/compare/README.md).
ENV APP_ENV=production APP_DEBUG=false LOG_CHANNEL=stderr \
    REVERB_APP_ID=bench REVERB_APP_KEY=bench-key REVERB_APP_SECRET=bench-secret \
    REVERB_SERVER_HOST=0.0.0.0 REVERB_SERVER_PORT=8080 \
    REVERB_SCALING_ENABLED=false
# PHP's own memory_limit would stop a server long before the container's memory does; every PHP contender runs with none (the container limit is the same for all).
CMD ["php", "-d", "memory_limit=-1", "artisan", "reverb:start", "--host=0.0.0.0", "--port=8080", "--no-interaction"]
