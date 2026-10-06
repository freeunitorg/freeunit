<?php
header('X-Pid: ' . getmypid());

echo 'before;';

if (!fastcgi_finish_request()) {
    error_log('Error in fastcgi_finish_request');
}

// The response is complete, and the context has no request.  flush() must
// not send anything, and must not use the request.
flush();

echo 'after;';

flush();

error_log('flush after finish: done');
?>
