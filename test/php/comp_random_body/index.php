<?php
/*
 * An incompressible body of the requested size, so that the compressed
 * response is roughly as large as the original and each request really does
 * move that many bytes of shared memory through the router.
 *
 * The digest travels in a header so the test can check the decompressed body
 * byte for byte without the two sides having to agree on the bytes.
 */
$n = (int) ($_GET['n'] ?? 1048576);
$body = random_bytes($n);
header('X-Body-Sha256: ' . hash('sha256', $body));
header('Content-Length: ' . strlen($body));
echo $body;
