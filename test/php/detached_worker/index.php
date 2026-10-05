<?php
/*
 * Answer the client, then keep the worker busy.  fastcgi_finish_request()
 * makes libunit send the last DATA message while the script runs on, which
 * is what puts the router's accounting and the real state of the worker out
 * of step.
 *
 * Query parameters, all optional:
 *
 *   ran=PATH     append the worker pid to PATH before answering -- proves
 *                the request executed, and how many times: a request that
 *                was cancelled while parked must not execute at all.
 *   hold=N       hold the worker for N seconds and then answer normally,
 *                with no fastcgi_finish_request().  The router's
 *                "limits": {"timeout"} deadline fires while the script still
 *                runs and nothing else ever reports that the worker is busy,
 *                so the router has to keep it out of the idle economy on its
 *                own until the answer arrives.
 *   before=N     hold the worker for N seconds *before* answering.  Past
 *                "limits": {"timeout"} the router fails the request while the
 *                script still runs, so the worker stays out of the idle
 *                economy for it and the detached report that follows is what
 *                ends that state -- unless the answer settles first, in which
 *                case the port is parked and the report takes it back out.
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
    file_put_contents($ran, (string) getmypid() . "\n", FILE_APPEND);
}

$hold = isset($_GET['hold']) ? (int) $_GET['hold'] : 0;

if ($hold > 0) {
    sleep($hold);

    $done = isset($_GET['done']) ? (string) $_GET['done'] : '';

    if ($done !== '') {
        file_put_contents($done, (string) getmypid());
    }

    echo "done";

    return;
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
