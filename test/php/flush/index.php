<?php
header('X-Flush: 1');
header('X-OB-Level: ' . ob_get_level());

if (isset($_GET['part1'])) {
    echo 'part1;';
}

flush();

$sent = headers_sent() ? 'true' : 'false';

sleep(isset($_GET['sleep']) ? (int) $_GET['sleep'] : 0);

echo "part2;headers_sent=$sent;";

flush();

echo 'part3;';
?>
