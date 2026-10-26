<?php
header('Content-Length: 0');
header('Query-String: ' . $_SERVER['QUERY_STRING']);
header('Request-Uri: ' . $_SERVER['REQUEST_URI']);
header('Unit-Rewritten-Path: ' . $_SERVER['UNIT_REWRITTEN_PATH']);
?>
