<?php
/*
 * test/test_schedules_php.py: logs "start" and "end" as JSON lines to
 * $SCHEDULE_LOG.  ?sleep=N, ?status=N; ?finish=1 calls
 * fastcgi_finish_request() and then sleeps, logging "detached_end".
 */

function record($event, $extra = [])
{
    $log = getenv('SCHEDULE_LOG');

    if ($log === false) {
        return;
    }

    $rec = array_merge(
        [
            'event' => $event,
            'time' => microtime(true),
            'pid' => getmypid(),
            'uri' => $_SERVER['REQUEST_URI'] ?? null,
            'query' => $_SERVER['QUERY_STRING'] ?? null,
            'host' => $_SERVER['HTTP_HOST'] ?? null,
            'user_agent' => $_SERVER['HTTP_USER_AGENT'] ?? null,
            'x_cron' => $_SERVER['HTTP_X_CRON'] ?? null,
            'server_name' => $_SERVER['SERVER_NAME'] ?? null,
            'server_port' => $_SERVER['SERVER_PORT'] ?? null,
            'remote_addr' => $_SERVER['REMOTE_ADDR'] ?? null,
            'method' => $_SERVER['REQUEST_METHOD'] ?? null,
        ],
        $extra
    );

    file_put_contents($log, json_encode($rec) . "\n", FILE_APPEND | LOCK_EX);
}

record('start');

$sleep = isset($_GET['sleep']) ? (float) $_GET['sleep'] : 0.0;
$status = isset($_GET['status']) ? (int) $_GET['status'] : 200;
$finish = isset($_GET['finish']) && $_GET['finish'] === '1';

$body = "ran " . ($_SERVER['REQUEST_URI'] ?? '') . "\n";

http_response_code($status);
header('Content-Type: text/plain');

if ($finish) {
    echo $body;
    record('end', ['status' => $status, 'finished_early' => true]);
    fastcgi_finish_request();

    if ($sleep > 0) {
        usleep((int) ($sleep * 1000000));
    }

    record('detached_end', ['status' => $status]);
    exit;
}

if ($sleep > 0) {
    usleep((int) ($sleep * 1000000));
}

record('end', ['status' => $status]);

echo $body;
