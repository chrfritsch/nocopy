#!/bin/sh
# PIE install from the committed tree (path repository), default and with each configure option.
set -e
export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null && apt-get -qq install -y --no-install-recommends libtool unzip git >/dev/null
curl -fsSL -o /usr/local/bin/pie https://github.com/php/pie/releases/download/1.5.5/pie.phar && chmod +x /usr/local/bin/pie
mkdir -p /tmp/n && cd /src && git ls-files -z | tar --null -T - -cf - | tar -xf - -C /tmp/n
cd /tmp && pie repository:add path /tmp/n >/dev/null
check() {
  pie uninstall -n chrfritsch/nocopy >/dev/null 2>&1 || true
  pie install chrfritsch/nocopy:@dev "$@" | grep -E "Configure complete|Install complete"
  php -d nocopy.shm_name=/pie-check -r 'nocopy_put("a", [1]); exit(nocopy_get("a") === [1] ? 0 : 1);'
  vk=$(php -d nocopy.shm_name=/pie-check -r 'echo (int) array_key_exists("nocopy.valkey", ini_get_all("nocopy"));')
  rm -f /dev/shm/pie-check.*
  echo "pie install $*: ok, valkey=$vk"
}
check
[ "$vk" = 1 ]
check --disable-nocopy-valkey
[ "$vk" = 0 ]
check --disable-nocopy-lto
