PHP_ARG_ENABLE([nocopy],
  [whether to enable nocopy support],
  [AS_HELP_STRING([--enable-nocopy],
    [Enable nocopy support])],
  [no])

PHP_ARG_ENABLE([nocopy-valkey],
  [whether nocopy can share its cache through Valkey],
  [AS_HELP_STRING([--disable-nocopy-valkey],
    [nocopy: build without the Valkey tier (nocopy.valkey), single host only])],
  [yes],
  [no])

AS_VAR_IF([PHP_NOCOPY], [no],, [
  AC_DEFINE([HAVE_NOCOPY], [1],
    [Define to 1 if the PHP extension 'nocopy' is available.])

  dnl shm_open: libc from glibc 2.34, librt before
  PHP_CHECK_FUNC(shm_open, rt)

  nocopy_sources="nocopy.c nocopy_arena.c nocopy_evict.c nocopy_lease.c nocopy_owner.c"
  AS_VAR_IF([PHP_NOCOPY_VALKEY], [no],, [
    AC_DEFINE([HAVE_NOCOPY_VALKEY], [1], [Define to 1 if nocopy has the Valkey tier.])
    nocopy_sources="$nocopy_sources nocopy_valkey.c"
    dnl TLS for nocopy.valkey (tls://): OpenSSL when found, else a build without it
    PKG_CHECK_MODULES([NOCOPY_OPENSSL], [openssl >= 1.1.1], [
      PHP_EVAL_INCLINE([$NOCOPY_OPENSSL_CFLAGS])
      PHP_EVAL_LIBLINE([$NOCOPY_OPENSSL_LIBS], [NOCOPY_SHARED_LIBADD])
      AC_DEFINE([HAVE_NOCOPY_TLS], [1], [Define to 1 if nocopy.valkey can use TLS.])
    ], [AC_MSG_WARN([OpenSSL not found: nocopy.valkey without TLS])])
  ])
  PHP_SUBST([NOCOPY_SHARED_LIBADD])

  PHP_NEW_EXTENSION([nocopy],
    [$nocopy_sources],
    [$ext_shared],,
    [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -fvisibility=hidden])
])
