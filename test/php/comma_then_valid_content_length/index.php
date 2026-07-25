<?php
/*
 * A rejected Content-Length followed by a syntactically valid one.  The later
 * value must not be reinstated as if it were the first: it disagrees with the
 * body just as the rejected list form did.
 */
header('Content-Length: 10, 200', false);
header('Content-Length: 50', false);
echo '0123456789';
