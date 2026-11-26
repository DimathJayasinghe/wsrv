<?php

// The documented `wsrv` connection (docs/LARAVEL_ECHO.md section 3), read from the environment.
return [
    'default' => env('BROADCAST_CONNECTION', 'wsrv'),
    'connections' => [
        'wsrv' => [
            'driver' => 'wsrv',
            'key' => env('WSRV_APP_KEY'),
            'secret' => env('WSRV_APP_SECRET'),
            'publish_url' => env('WSRV_PUBLISH_URL', 'http://127.0.0.1:9200'),
            'publish_token_file' => env('WSRV_PUBLISH_TOKEN_FILE'),
            'ipc_endpoint' => env('WSRV_IPC', '/run/wsrv/ipc.sock'),
            'encryption_master_key_base64' => env('WSRV_ENCRYPTION_KEY'),
        ],
    ],
];
