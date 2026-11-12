<?php
if (ini_get('nocopy.resp') === false) die('skip built without the RESP tier');
if (!@fsockopen('127.0.0.1', 6379)) die('skip no RESP on 127.0.0.1:6379');
if (!getenv('NOCOPY_SO')) die('skip NOCOPY_SO not set');
