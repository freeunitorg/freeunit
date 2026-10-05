<?php
/*
 * A body small enough that libunit delivers it in an ordinary port message
 * rather than through shared memory, so the response buffer the router hands
 * the compressor is plain memory rather than port-mmap.  See issue #162.
 */
$body = str_repeat('A', 64);
header('Content-Length: ' . strlen($body));
echo $body;
