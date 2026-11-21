<?php
if (isset($_GET['memory_limit'])) {
    ini_set('memory_limit', $_GET['memory_limit']);
}

header('X-Session: ' . (extension_loaded('session') ? '1' : '0'));
header('X-Upload-Progress: ' . ini_get('session.upload_progress.enabled'));
header('X-Max-Multipart: ' . var_export(ini_get('max_multipart_body_parts'),
                                        true));
header('X-Max-File-Uploads: ' . ini_get('max_file_uploads'));
header('X-Max-Input-Vars: ' . ini_get('max_input_vars'));
header('X-Post-Max-Size: ' . ini_get('post_max_size'));
header('X-Memory-Limit: ' . ini_get('memory_limit'));
header('X-Strict-Mode: ' . ini_get('session.use_strict_mode'));
header('X-Cookie-Httponly: ' . ini_get('session.cookie_httponly'));
header('X-Cookie-Samesite: ' . ini_get('session.cookie_samesite'));
?>
