<?php
/*
 * The body is the pid of the worker, its memory_limit and PHP_VERSION_ID.  The
 * pid shows that the router replaced the worker after a failed request
 * startup.  The limit shows that the option of the test was applied.  The
 * version tells the test whether the engine can end the process by itself.
 */
echo getmypid(), ' ', ini_get('memory_limit'), ' ', PHP_VERSION_ID;
