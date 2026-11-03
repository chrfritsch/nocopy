dnl Autotools config.m4 for PHP extension stash

dnl Comments in this file start with the string 'dnl' (discard to next line).
dnl Remove where necessary.

dnl If extension references and depends on an external library package, use
dnl the '--with-stash' configure option:
dnl PHP_ARG_WITH([stash],
dnl   [for stash support],
dnl   [AS_HELP_STRING([--with-stash],
dnl     [Include stash support])])

dnl Otherwise use the '--enable-stash' configure option:
PHP_ARG_ENABLE([stash],
  [whether to enable stash support],
  [AS_HELP_STRING([--enable-stash],
    [Enable stash support])],
  [no])

AS_VAR_IF([PHP_STASH], [no],, [
  dnl This section is executed when extension is enabled with one of the above
  dnl configure options. Adjust and add tests here.

  dnl
  dnl Use and adjust this code block if extension depends on external library
  dnl package which supports pkg-config.
  dnl
  dnl Find library package with pkg-config.
  dnl PKG_CHECK_MODULES([LIBFOO], [foo])
  dnl
  dnl Or if you need to check for a particular library version with pkg-config,
  dnl you can use comparison operators. For example:
  dnl PKG_CHECK_MODULES([LIBFOO], [foo >= 1.2.3])
  dnl PKG_CHECK_MODULES([LIBFOO], [foo < 3.4])
  dnl PKG_CHECK_MODULES([LIBFOO], [foo = 1.2.3])
  dnl
  dnl Add library compilation and linker flags to extension.
  dnl PHP_EVAL_INCLINE([$LIBFOO_CFLAGS])
  dnl PHP_EVAL_LIBLINE([$LIBFOO_LIBS], [STASH_SHARED_LIBADD])
  dnl
  dnl Check for library and symbol presence.
  dnl LIBNAME=stash # you may want to change this
  dnl LIBSYMBOL=stash # you most likely want to change this
  dnl
  dnl If you need to check for a particular library function (e.g. a conditional
  dnl or version-dependent feature) and you are using pkg-config:
  dnl PHP_CHECK_LIBRARY([$LIBNAME], [$LIBSYMBOL],
  dnl   [AC_DEFINE([HAVE_STASH_FEATURE], [1],
  dnl     [Define to 1 if stash has the 'FEATURE'.])],
  dnl   [AC_MSG_FAILURE([FEATURE not supported by your stash library.])],
  dnl   [$LIBFOO_LIBS])
  dnl

  dnl
  dnl Or use and adjust this code block if extension depends on external library
  dnl package, which does not support pkg-config.
  dnl
  dnl Path to library package can be given as parameter (--with-stash=<DIR>)
  dnl SEARCH_PATH="/usr/local /usr" # you might want to change this
  dnl SEARCH_FOR="/include/stash.h" # you most likely want to change this
  dnl AS_IF([test -r $PHP_STASH/$SEARCH_FOR],
  dnl   [STASH_DIR=$PHP_STASH],
  dnl   [
  dnl     for i in $SEARCH_PATH; do
  dnl       AS_IF([test -r $i/$SEARCH_FOR],
  dnl         [STASH_DIR=$i; break;])
  dnl     done
  dnl   ])
  dnl
  dnl AC_MSG_CHECKING([for stash library package])
  dnl AS_VAR_IF([STASH_DIR],, [
  dnl   AC_MSG_RESULT([not found])
  dnl   AC_MSG_ERROR([Please reinstall the stash library package])
  dnl ], [AC_MSG_RESULT([found in $STASH_DIR])])
  dnl
  dnl Add include flag where library package headers are located on the system.
  dnl PHP_ADD_INCLUDE([$STASH_DIR/include])
  dnl
  dnl Check for library and symbol presence.
  dnl LIBNAME=stash # you may want to change this
  dnl LIBSYMBOL=stash # you most likely want to change this
  dnl
  dnl If you need to check for a particular library function (e.g. a conditional
  dnl or version-dependent feature) and you are not using pkg-config:
  dnl PHP_CHECK_LIBRARY([$LIBNAME], [$LIBSYMBOL], [
  dnl     PHP_ADD_LIBRARY_WITH_PATH([$LIBNAME],
  dnl       [$STASH_DIR/$PHP_LIBDIR],
  dnl       [STASH_SHARED_LIBADD])
  dnl     AC_DEFINE([HAVE_STASH_FEATURE], [1],
  dnl       [Define to 1 if stash has the 'FEATURE'.])
  dnl   ],
  dnl   [AC_MSG_FAILURE([FEATURE not supported by your stash library.])],
  dnl   [-L$STASH_DIR/$PHP_LIBDIR -lm])
  dnl

  dnl Add linked libraries flags for shared extension to the generated Makefile.
  dnl PHP_SUBST([STASH_SHARED_LIBADD])

  dnl Define a preprocessor macro to indicate that this PHP extension can
  dnl be dynamically loaded as a shared module or is statically built into PHP.
  AC_DEFINE([HAVE_STASH], [1],
    [Define to 1 if the PHP extension 'stash' is available.])

  dnl Configure extension sources and compilation flags.
  PHP_NEW_EXTENSION([stash],
    [stash.c stash_apcu.c],
    [$ext_shared],,
    [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -fvisibility=hidden])
])
