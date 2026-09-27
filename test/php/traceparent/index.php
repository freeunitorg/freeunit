<?php
header('X-Seen-Traceparent: ' . ($_SERVER['HTTP_TRACEPARENT'] ?? ''));
header('Content-Length: 0');
