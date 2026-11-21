<?php
/*
 * No Content-Length, and a body far below a "min_length" of 1024: five
 * writes of 40 bytes, each its own message to the router.
 */
for ($i = 0; $i < 5; $i++) {
    echo str_repeat('C', 40);
    flush();
}
