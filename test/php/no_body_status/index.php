<?php

$status = isset($_GET['status']) ? (int) $_GET['status'] : 204;

http_response_code($status);

if (isset($_GET['cl'])) {
    header('Content-Length: 10');
}

if (isset($_GET['te'])) {
    header('Transfer-Encoding: chunked');
}

echo 'SMUGGLEDXX';
