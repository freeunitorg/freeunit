<?php
/*
 * Large enough to travel through shared memory, so the compressor receives a
 * port-mmap buffer.  This is the case that works today; it is the control for
 * the small-body case staged in #162.
 */
$body = str_repeat('A', 100000);
header('Content-Length: ' . strlen($body));
echo $body;
