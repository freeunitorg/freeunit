<?php
/*
 * A body small enough that libunit delivers it in an ordinary port message
 * rather than through shared memory, so the response buffer the router hands
 * the compressor is plain memory rather than port-mmap.  See issue #162.
 * libunit sends a part in plain memory when it fits one port queue item
 * (NXT_UNIT_QUEUE_PLAIN_SIZE, 15 bytes); a larger part takes shared memory
 * whenever it can, so the body has to stay below that.
 */
$body = str_repeat('A', 8);
header('Content-Length: ' . strlen($body));
echo $body;
