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

PHP_ARG_ENABLE([nocopy-lto],
  [whether to build nocopy with link-time optimization],
  [AS_HELP_STRING([--disable-nocopy-lto],
    [nocopy: build without -flto (shared builds use it when the compiler links with it)])],
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
    dnl vendored libvalkey: the same list as scripts/vendor-libvalkey.sh
    lv_sources="adlist.c alloc.c async.c conn.c dict.c net.c read.c sds.c sockcompat.c valkey.c"
    dnl its public API stays out of nocopy.so's exports (its visibility.h would mark it default)
    lv_cflags="-DVALKEY_USE_THREADS -DVALKEY_VISIBILITY_H -DLIBVALKEY_API="
    dnl TLS for nocopy.valkey (tls://): OpenSSL when found, else a build without it
    PKG_CHECK_MODULES([NOCOPY_OPENSSL], [openssl >= 1.1.1], [
      PHP_EVAL_INCLINE([$NOCOPY_OPENSSL_CFLAGS])
      PHP_EVAL_LIBLINE([$NOCOPY_OPENSSL_LIBS], [NOCOPY_SHARED_LIBADD])
      AC_DEFINE([HAVE_NOCOPY_TLS], [1], [Define to 1 if nocopy.valkey can use TLS.])
      lv_sources="$lv_sources tls.c"
    ], [AC_MSG_WARN([OpenSSL not found: nocopy.valkey without TLS])])
  ])
  dnl LTO inlines the arena into the fetch and store paths (ab.sh: fetches +2-4%, inc +10%); shared only, PHP itself links without it
  nocopy_lto=
  AS_VAR_IF([PHP_NOCOPY_LTO], [no],, [AS_VAR_IF([ext_shared], [no],, [
    for f in -flto=auto -flto; do
      AC_MSG_CHECKING([whether $CC links with $f])
      nocopy_save_cflags=$CFLAGS nocopy_save_ldflags=$LDFLAGS
      CFLAGS="$CFLAGS $f" LDFLAGS="$LDFLAGS $f"
      AC_LINK_IFELSE([AC_LANG_PROGRAM([], [])], [nocopy_lto=$f])
      CFLAGS=$nocopy_save_cflags LDFLAGS=$nocopy_save_ldflags
      AS_VAR_IF([nocopy_lto], [], [AC_MSG_RESULT([no])], [AC_MSG_RESULT([yes]); break])
    done
  ])])
  dnl -Wc,: libtool drops a bare -flto from the link line for clang (whose objects are then bitcode)
  AS_VAR_IF([nocopy_lto], [],, [NOCOPY_SHARED_LIBADD="$NOCOPY_SHARED_LIBADD -Wc,$nocopy_lto"])
  PHP_SUBST([NOCOPY_SHARED_LIBADD])

  PHP_NEW_EXTENSION([nocopy],
    [$nocopy_sources],
    [$ext_shared],,
    [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -fvisibility=hidden $nocopy_lto $lv_cflags])

  AS_VAR_IF([PHP_NOCOPY_VALKEY], [no],, [
    lv=third_party/libvalkey
    PHP_ADD_INCLUDE([$ext_srcdir/$lv/include])
    PHP_ADD_INCLUDE([$ext_srcdir/$lv/include/valkey])
    PHP_ADD_INCLUDE([$ext_srcdir/$lv/src])
    PHP_ADD_BUILD_DIR([$ext_builddir/$lv/src])
    dnl their code, their warnings: -w; PHP's -D_GNU_SOURCE would give them GNU strerror_r (empty error messages)
    lv_flags="-fvisibility=hidden $lv_cflags -U_GNU_SOURCE -w"
    AS_VAR_IF([ext_shared], [no],
      [PHP_ADD_SOURCES([PHP_EXT_DIR([nocopy])/$lv/src], [$lv_sources], [$lv_flags])],
      [PHP_ADD_SOURCES_X([PHP_EXT_DIR([nocopy])/$lv/src], [$lv_sources], [$lv_flags], [shared_objects_nocopy], [yes])])
  ])
])
