<?php

use Illuminate\Support\Facades\Broadcast;

Broadcast::channel('chat.{id}', fn ($user, $id) => true);
Broadcast::channel('room.{id}', fn ($user, $id) => ['name' => $user->name]);   // a presence channel: the member's info
Broadcast::channel('secret.{id}', fn ($user, $id) => true);                     // also serves private-encrypted-secret.{id}
Broadcast::channel('forbidden.{id}', fn ($user, $id) => false);
