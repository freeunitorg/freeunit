<?php
echo 'ct=' . (isset($_SERVER['CONTENT_TYPE']) ? $_SERVER['CONTENT_TYPE'] : 'none');
echo '|post=' . (empty($_POST) ? 'empty' : implode(',', array_keys($_POST)));
if (isset($_GET['raw'])) {
    echo '|raw=' . file_get_contents('php://input');
}
