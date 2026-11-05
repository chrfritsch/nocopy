<?php
if (ini_get('nocopy.valkey') === false) die('skip built without the Valkey tier');
if (!@fsockopen('127.0.0.1', 6379)) die('skip no Valkey on 127.0.0.1:6379');
if (!getenv('NOCOPY_SO')) die('skip NOCOPY_SO not set');
