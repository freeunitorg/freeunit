<?php
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * A response that the application coded itself.  It is not identity, so a
 * client that refused identity can take it, and Unit must not negotiate it
 * again or code it twice.
 */
$body = gzencode(str_repeat('A', 100000));
header('Content-Type: text/html');
header('Content-Encoding: gzip');
header('Content-Length: ' . strlen($body));
echo $body;
