<?php
$fds = shell_exec('ls -l /proc/self/fd 2>/dev/null');

echo str_contains((string) $fds, 'fd.php') ? 'visible' : 'hidden';
