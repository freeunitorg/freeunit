<?php
$names = [
    'SERVER_SOFTWARE', 'SERVER_PROTOCOL', 'PHP_SELF', 'PATH_INFO',
    'SCRIPT_NAME', 'SCRIPT_FILENAME', 'DOCUMENT_ROOT', 'REQUEST_METHOD',
    'REQUEST_URI', 'QUERY_STRING', 'REMOTE_ADDR', 'SERVER_ADDR',
    'SERVER_NAME', 'SERVER_PORT', 'HTTPS', 'CONTENT_LENGTH', 'CONTENT_TYPE',
    'HTTP_X_PROBE',
];

$vars = ['pid' => getmypid()];

foreach ($names as $name) {
    if (array_key_exists($name, $_SERVER)) {
        $vars[$name] = $_SERVER[$name];
    }
}

/* The next request must not see any of these changes. */
$_SERVER['REQUEST_METHOD'] = 'CHANGED';
$_SERVER['SERVER_SOFTWARE'] .= '-changed';
$_SERVER['SCRIPT_NAME'] .= '-changed';
$_SERVER['DOCUMENT_ROOT'] .= '-changed';
$ref = &$_SERVER['SERVER_PROTOCOL'];
$ref .= '-changed';
unset($_SERVER['QUERY_STRING']);

if (isset($_SERVER['HTTPS'])) {
    $_SERVER['HTTPS'] .= '-changed';
}

header('Content-Type: application/json');
echo json_encode($vars);
