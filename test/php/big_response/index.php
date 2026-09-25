<?php
/*
 * Emits ?mb=<n> mebibytes of 'x' in 64 KiB writes.  Each write goes through
 * the SAPI's ub_write -> nxt_unit_response_write(), which allocates the bytes
 * out of the application's outgoing shared-memory segments, so a response
 * larger than the application's "limits"/"shm" budget cannot be in flight all
 * at once: the allocator runs the segment's free map dry and Unit's
 * application library reports the out-of-shared-memory condition to the
 * router (nxt_unit_send_oosm()).
 */
$mb = isset($_GET['mb']) ? (int) $_GET['mb'] : 32;

header('Content-Type: text/plain');
header('Content-Length: ' . ($mb * 1024 * 1024));

$chunk = str_repeat('x', 65536);

for ($i = 0; $i < $mb * 16; $i++) {
    echo $chunk;
}
