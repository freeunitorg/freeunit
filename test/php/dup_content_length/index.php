<?php
/*
 * Emit two disagreeing Content-Length headers (replace=false keeps both).
 * The router must forward neither and frame the body by chunked encoding.
 */
header('Content-Length: 10', false);
header('Content-Length: 200', false);
echo '0123456789';
