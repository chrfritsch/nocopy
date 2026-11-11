# Linux build + test image. Source is mounted at /src, built out of tree in /build:
#   docker build --build-arg PHP=8.5 -t nocopy-linux:8.5 docker
#   docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/test.sh
ARG PHP=8.5
FROM php:${PHP}-fpm
# PECL releases as tarballs pinned by checksum (no pecl, no PIE): bump version and sha256 together
ADD --checksum=sha256:ca9c1820810a168786f8048a4c3f8c9e3fd941407ad1553259fb2e30b5f057bf https://pecl.php.net/get/apcu-5.1.28.tgz /tmp/
ADD --checksum=sha256:91da821443db125282a6aea039f24588dd28ff5d71e8187f6ecc41165bceafbc https://pecl.php.net/get/igbinary-3.2.17RC1.tgz /tmp/
RUN apt-get update && apt-get install -y --no-install-recommends nginx apache2-utils curl valkey-server git libssl-dev pkg-config clang clang-tidy libclang-rt-dev faketime && rm -rf /var/lib/apt/lists/* \
 && docker-php-ext-install pcntl \
 && cd /tmp && tar xzf apcu-5.1.28.tgz && tar xzf igbinary-3.2.17RC1.tgz && docker-php-ext-install /tmp/apcu-5.1.28 /tmp/igbinary-3.2.17RC1 \
 && rm -rf /tmp/apcu-* /tmp/igbinary-* /tmp/package*.xml && rm -f /usr/local/etc/php/conf.d/*apcu* /usr/local/etc/php/conf.d/*igbinary* \
 && echo "apcu: $(find /usr/local/lib/php/extensions -name apcu.so)"
