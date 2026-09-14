<?php
/*
 * Answer the client, then keep the worker busy.  fastcgi_finish_request()
 * makes libunit send the last DATA message while the script runs on, which
 * is what puts the router's accounting and the real state of the worker out
 * of step.
 *
 * Query parameters, all optional:
 *
 *   ran=PATH     write PATH before answering -- proves the request executed
 *                at all, which a request that was cancelled while parked
 *                must not do.
 *   before=N     hold the worker for N seconds *before* answering.  Past
 *                "limits": {"timeout"} the router fails the request and
 *                parks the worker as idle, so the detached report that
 *                follows arrives for a port that is already in idle_ports.
 *   sleep=N      hold the worker for N seconds after answering.
 *   done=PATH    write PATH after the sleep -- proves the detached work ran
 *                to completion rather than being killed part-way.
 *   after=exit   call exit() once the detached work is done.
 *   after=fatal  raise a fatal error once the detached work is done.
 *
 * The markers are written with file_put_contents() rather than touch() so a
 * partially written file cannot read as a complete one.
 */

$ran = isset($_GET['ran']) ? (string) $_GET['ran'] : '';

if ($ran !== '') {
    file_put_contents($ran, (string) getmypid());
}

$before = isset($_GET['before']) ? (int) $_GET['before'] : 0;

if ($before > 0) {
    sleep($before);
}

echo "done";

fastcgi_finish_request();

$sleep = isset($_GET['sleep']) ? (int) $_GET['sleep'] : 0;

if ($sleep > 0) {
    sleep($sleep);
}

$done = isset($_GET['done']) ? (string) $_GET['done'] : '';

if ($done !== '') {
    file_put_contents($done, (string) getmypid());
}

$after = isset($_GET['after']) ? (string) $_GET['after'] : '';

if ($after === 'exit') {
    exit(0);
}

if ($after === 'fatal') {
    /* An uncaught Error: a fatal path that runs after the response went out. */
    throw new Error('detached fatal');
}
