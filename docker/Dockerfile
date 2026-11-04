# Linux build + test image. Source is mounted at /src, built out of tree in /build:
#   docker build --build-arg PHP=8.5 -t nocopy-linux:8.5 docker
#   docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/test.sh
ARG PHP=8.5
FROM php:${PHP}-fpm
RUN apt-get update && apt-get install -y --no-install-recommends nginx apache2-utils curl && rm -rf /var/lib/apt/lists/* \
 && docker-php-ext-install pcntl \
 && pecl install apcu && rm -f /usr/local/etc/php/conf.d/*apcu* \
 && echo "apcu: $(find /usr/local/lib/php/extensions -name apcu.so)"
