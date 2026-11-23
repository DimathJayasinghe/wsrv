# Swoole (the official image). One worker, so a broadcast reaches every connection.
FROM phpswoole/swoole:php8.3
WORKDIR /app
COPY server.php /app/server.php
# PHP's own memory_limit would stop a server long before the container's memory does; every PHP contender runs with none (the container limit is the same for all).
CMD ["php", "-d", "memory_limit=-1", "/app/server.php"]
