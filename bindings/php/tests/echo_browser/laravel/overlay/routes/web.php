<?php

use App\Events\Dyn;
use Illuminate\Auth\GenericUser;
use Illuminate\Http\Request;
use Illuminate\Support\Facades\Auth;
use Illuminate\Support\Facades\Route;

// Two users, told apart by a header (the page runs both in one browser, so one cookie would not do).
config(['auth.guards.demo' => ['driver' => 'demo'], 'auth.defaults.guard' => 'demo']);
Auth::viaRequest('demo', function (Request $request) {
    $users = ['ada' => ['id' => 1, 'name' => 'Ada'], 'bob' => ['id' => 2, 'name' => 'Bob']];
    $user = $users[$request->header('X-User', '')] ?? null;
    return $user === null ? null : new GenericUser($user);
});

Route::get('/', function () {
    $html = file_get_contents(base_path('echo-test/index.html'));
    return response(str_replace(
        ['__KEY__', '__WS_PORT__', '__CSRF__'],
        [env('WSRV_APP_KEY'), env('PUBLIC_WS_PORT'), csrf_token()],
        $html
    ));
});

// The server side of the scenario: a real event through broadcast(), the way an application does it.
Route::post('/emit', function (Request $request) {
    broadcast(new Dyn((string) $request->input('channel'), (string) $request->input('event'), (array) $request->input('data', [])))->toOthers();
    return response()->json(['ok' => true]);
});

// Plumbing between the page and the script that started everything (run.sh).
Route::post('/restart', function () {
    touch(env('WSRV_RESTART_FLAG'));
    return response()->json(['ok' => true]);
});
Route::get('/restarted', fn () => response()->json(['done' => is_file(env('WSRV_RESTARTED_FLAG'))]));
Route::post('/result', function (Request $request) {
    file_put_contents(env('WSRV_RESULT_FILE'), json_encode($request->all()));
    return response()->json(['ok' => true]);
});
