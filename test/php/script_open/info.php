<?php
echo implode(', ', array_map('basename', get_included_files()));
