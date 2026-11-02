<?php
/*
 * No Content-Length: the body is echoed in parts, as most PHP scripts do,
 * so the router does not know its length and sends it chunked (#856).
 */
for ($i = 0; $i < 10; $i++) {
    echo str_repeat('B', 10000);
    flush();
}
