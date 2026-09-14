<?php
/*
 * Answer the client, then keep the worker busy.  fastcgi_finish_request()
 * makes libunit send the last DATA message while the script runs on, which
 * is what puts the router's accounting and the real state of the worker out
 * of step.
 */

echo "done";

fastcgi_finish_request();

$sleep = isset($_GET['sleep']) ? (int) $_GET['sleep'] : 0;

if ($sleep > 0) {
    sleep($sleep);
}
