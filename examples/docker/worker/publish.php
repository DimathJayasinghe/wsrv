<?php
// Publish to a room from any process: php /app/publish.php <room> <message>
// This is what a controller, a queue job or a cron script does: one short HTTP call, no connection to keep.
require __DIR__ . '/vendor/autoload.php';

use Wsrv\PublishFailed;
use Wsrv\Publisher;

[$script, $room, $message] = $argv + [null, null, null];
if ($room === null || $message === null) {
    fwrite(STDERR, "usage: php publish.php <room> <message>\n");
    exit(2);
}
$publisher = new Publisher(getenv('WSRV_PUBLISH_URL') ?: 'http://wsrvd:9200', trim((string) file_get_contents('/run/wsrv/publish.token')));
try {
    $r = $publisher->publish($room, $message);
    echo "subscribers=$r->subscribers accepted=$r->accepted refused=$r->refused\n";
} catch (PublishFailed $e) {
    fwrite(STDERR, 'publish failed: ' . $e->getMessage() . ($e->isRetryable() ? " (worth retrying)\n" : "\n"));
    exit(1);
}
