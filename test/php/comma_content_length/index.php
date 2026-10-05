<?php
/*
 * The list form of duplicate Content-Length: one field, two comma-joined
 * lengths.  Reaches a downstream parser exactly as two separate headers do.
 */
header('Content-Length: 10, 200');
echo '0123456789';
