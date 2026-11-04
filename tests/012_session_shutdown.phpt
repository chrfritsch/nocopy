--TEST--
nocopy: zero-copy values in $_SESSION stay pinned while the session is written at shutdown
--EXTENSIONS--
nocopy
session
--INI--
apc.shm_size=4M
nocopy.request_scope=1
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
class Handler implements SessionHandlerInterface, SessionIdInterface, SessionUpdateTimestampHandlerInterface {
    function create_sid(): string { return 'sid'; }
    function validateId($id): bool { return true; }
    function updateTimestamp($id, $data): bool { return true; }
    function open($p, $n): bool { return true; }
    function close(): bool { return true; }
    function read($id): string|false { return ''; }
    function write($id, $data): bool { echo str_contains($data, str_repeat('a', 40000)) ? "intact\n" : "corrupt\n"; return true; }
    function destroy($id): bool { return true; }
    function gc($max): int|false { return 0; }
}
class Evil {
    function __serialize(): array { // runs in session's RSHUTDOWN, after nocopy's
        nocopy_delete('big');
        nocopy_put('fill', str_repeat('Z', 40000)); // reuses big's segment unless it is still pinned
        return [];
    }
}
nocopy_put('big', [str_repeat('a', 40000)]);
session_set_save_handler(new Handler, false); // write in RSHUTDOWN, like the files handler
session_start();
$_SESSION['a'] = new Evil;
$_SESSION['b'] = nocopy_get('big');
echo "done\n";
?>
--EXPECT--
done
intact
