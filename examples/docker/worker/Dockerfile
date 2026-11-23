FROM php:8.3-cli
COPY --from=composer:2 /usr/bin/composer /usr/bin/composer
WORKDIR /app
RUN composer require --no-interaction --no-progress "wsrv/client:^0.1@rc" \
 && rm /usr/bin/composer
CMD ["php", "/app/worker.php"]
