--TEST--
nocopy.resp over TLS: verified peer (name and IP), client certificate, the listener's invalidations; each failure names its cause
--EXTENSIONS--
nocopy
--SKIPIF--
<?php
require __DIR__ . '/resp_skip.inc';
if (!is_executable('/usr/bin/openssl') || !($vs = trim((string) shell_exec('command -v valkey-server || command -v redis-server')))) die('skip needs openssl and valkey-server');
require __DIR__ . '/php.inc';
if (str_contains(php_run('nocopy_get(1);', ['nocopy.resp' => 'tls://localhost:1']), 'no TLS')) die('skip built without OpenSSL');
?>
--INI--
nocopy.shm_name=/nocopy-t063
--CONFLICTS--
resp
--FILE--
<?php
require_once __DIR__ . '/php.inc';
$d = sys_get_temp_dir() . '/nocopy-t063';
@mkdir($d);
$ssl = fn (string $args) => shell_exec("cd $d && /usr/bin/openssl $args 2>/dev/null");
$ssl('req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=ca -keyout ca.key -out ca.crt');
$ssl('req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=other -keyout other.key -out other.crt');
foreach (['server' => 'subjectAltName=DNS:localhost', 'client' => 'extendedKeyUsage=clientAuth'] as $n => $ext) {
    $ssl("req -newkey rsa:2048 -nodes -subj /CN=$n -keyout $n.key -out $n.csr");
    file_put_contents("$d/$n.ext", $ext);
    $ssl("x509 -req -in $n.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 1 -extfile $n.ext -out $n.crt");
}
file_put_contents("$d/bad.key", "not a key\n");
$vs = trim(shell_exec('command -v valkey-server || command -v redis-server'));
// env -u: ./dev asan preloads libasan, resp crashes under it
shell_exec("env -u LD_PRELOAD $vs --port 6391 --tls-port 6390 --tls-cert-file $d/server.crt --tls-key-file $d/server.key --tls-ca-cert-file $d/ca.crt --tls-auth-clients yes --save '' --daemonize yes --pidfile $d/vk.pid");
usleep(300000);

function child(string $url, array $ini, string $code): string
{
    $ini += ['nocopy.resp_tls_ca_file' => "$GLOBALS[d]/ca.crt", 'nocopy.resp_tls_cert_file' => "$GLOBALS[d]/client.crt", 'nocopy.resp_tls_key_file' => "$GLOBALS[d]/client.key"];
    static $n = 0;
    $ini = ['nocopy.shm_name' => '/nocopy-t063-' . ++$n] + $ini + ['nocopy.resp' => $url, 'nocopy.resp_prefix' => 't063:']; // one arena per child: its listener keeps that child's settings
    $code = '$GLOBALS["VK_PORT"] = 6391; require ' . var_export(__DIR__ . '/resp.inc', true) . '; ' . $code;
    return trim(preg_replace('/ in Command line code on line \d+/', '', php_run($code, $ini)));
}
$use = 'var_dump(nocopy_put("k", "v"));';
echo "verified, client cert:\n", child('tls://localhost:6390', [], 'nocopy_put("k", "v"); vk("SET", "t063:k", "7"); settle(); var_dump(nocopy_get("k"), nocopy_shared_with(), nocopy_stats()["resp_listener"] > 0);'), "\n";
echo "rediss:// scheme:\n", child('rediss://localhost:6390', [], $use), "\n";
echo "unknown CA:\n", child('tls://localhost:6390', ['nocopy.resp_tls_ca_file' => "$d/other.crt"], $use), "\n";
echo "unknown CA, verify off:\n", child('tls://localhost:6390', ['nocopy.resp_tls_ca_file' => "$d/other.crt", 'nocopy.resp_tls_verify' => 0], $use), "\n";
echo "IP not in the certificate:\n", child('tls://127.0.0.1:6390', [], $use), "\n";
echo "no client certificate:\n", child('tls://localhost:6390', ['nocopy.resp_tls_cert_file' => '', 'nocopy.resp_tls_key_file' => ''], $use), "\n";
echo "bad key file:\n", child('tls://localhost:6390', ['nocopy.resp_tls_key_file' => "$d/bad.key"], $use), "\n";
echo "plain to the TLS port:\n", child('tcp://localhost:6390', ['nocopy.resp_timeout' => 0.3], $use), "\n";
?>
--CLEAN--
<?php
$d = sys_get_temp_dir() . '/nocopy-t063';
if (is_file("$d/vk.pid")) {
    posix_kill((int) file_get_contents("$d/vk.pid"), 15);
}
usleep(200000);
array_map('unlink', glob("$d/*"));
@rmdir($d);
?>
<?php array_map('unlink', glob('/dev/shm/nocopy-t063*')); ?>
--EXPECTF--
verified, client cert:
int(7)
string(4) "resp"
bool(true)
rediss:// scheme:
bool(true)
unknown CA:
Warning: nocopy_put(): nocopy: cannot connect to RESP (localhost): self-signed certificate in certificate chain
bool(false)
unknown CA, verify off:
bool(true)
IP not in the certificate:
Warning: nocopy_put(): nocopy: cannot connect to RESP (127.0.0.1): IP address mismatch
bool(false)
no client certificate:
Warning: nocopy_put(): nocopy: RESP handshake failed: %s
bool(false)
bad key file:
Warning: PHP Startup: nocopy: nocopy.resp_tls_key_file: %s in Unknown on line 0

Warning: nocopy_put(): nocopy: cannot connect to RESP (localhost): nocopy.resp_tls_key_file: %s
bool(false)
plain to the TLS port:
Warning: nocopy_put(): nocopy: %s
bool(false)
