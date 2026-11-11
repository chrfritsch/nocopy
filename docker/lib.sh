# Sourced by the in-container scripts (. /src/docker/lib.sh): the paths, php flags and services they share.
SO=/build/modules/nocopy.so

apcu_so() { find /usr/local/lib/php/extensions -name apcu.so; }

# php flags: nocopy loaded on an arena of its own, /nocopy-<name>, then nocopy.* settings (shm_size=64M ...). SO: another build's module.
nocopy_flags() {
  _n=$1; shift
  printf '%s' "-d extension=$SO -d nocopy.shm_name=/nocopy-$_n"
  for _s in "$@"; do printf ' -d nocopy.%s' "$_s"; done
}

shm_rm() { rm -f "/dev/shm/nocopy-$1".*; }

# A Valkey on 127.0.0.1:6379 (the 05x tests, the tier's smoke tests) unless one answers; nothing where it is not installed.
valkey_up() {
  command -v valkey-server >/dev/null || return 0
  valkey-cli ping >/dev/null 2>&1 || valkey-server --daemonize yes --save '' --appendonly no >/dev/null
}

# After an ASan build. php itself is not instrumented: preload the runtime, plain malloc so emalloc overflows are seen, no leak report (php frees lazily).
asan_env() { export LD_PRELOAD="$(cc -print-file-name=libasan.so)" USE_ZEND_ALLOC=0 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1; }

# PHP's run-tests.php with nocopy loaded; args: its options and the tests. TIMEOUT: seconds before the run is killed (exit 124).
run_tests() {
  ${TIMEOUT:+timeout "$TIMEOUT"} env TEST_PHP_EXECUTABLE="$(command -v php)" NO_INTERACTION=1 \
    php "$(php-config --prefix)/lib/php/build/run-tests.php" -n $(nocopy_flags runner) -q "$@"
}
