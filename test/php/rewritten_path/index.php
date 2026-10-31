<?php
$server = $_SERVER;
$result = array(
    'present' => array_key_exists('UNIT_REWRITTEN_PATH', $server),
    'path' => isset($server['UNIT_REWRITTEN_PATH'])
        ? $server['UNIT_REWRITTEN_PATH'] : null,
    'request_uri' => $server['REQUEST_URI'],
    'query' => $server['QUERY_STRING'],
    'php_self' => $server['PHP_SELF'],
    'path_info' => isset($server['PATH_INFO']) ? $server['PATH_INFO'] : null,
    'http_path' => isset($server['HTTP_UNIT_REWRITTEN_PATH'])
        ? $server['HTTP_UNIT_REWRITTEN_PATH'] : null,
    'filter_available' => function_exists('filter_input'),
);

if ($result['filter_available']) {
    // INPUT_SERVER must keep its original value after $_SERVER is changed.
    $_SERVER['UNIT_REWRITTEN_PATH'] = '/changed-in-php';
    $result['filter_path'] = filter_input(INPUT_SERVER, 'UNIT_REWRITTEN_PATH');
    $result['filter_request_uri'] = filter_input(INPUT_SERVER, 'REQUEST_URI');
}

$body = json_encode($result);
header('Content-Type: application/json');
header('Content-Length: ' . strlen($body));
echo $body;
?>
