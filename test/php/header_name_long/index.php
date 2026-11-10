<?php
header(str_repeat('X', (int) ($_SERVER['HTTP_X_LENGTH'] ?? 300)) . ': value');
echo 'body';
?>
