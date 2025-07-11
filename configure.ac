dnl wavpack 5.8.1 configure.ac

AC_INIT([wavpack], [5.8.1], [bryant@wavpack.com])
AC_CONFIG_SRCDIR([src/pack.c])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([-Wall 1.15 serial-tests subdir-objects foreign no-dist-gzip dist-xz])
AM_MAINTAINER_MODE

LIBWAVPACK_MAJOR=5
LIBWAVPACK_MINOR=8
LIBWAVPACK_MICRO=1
LIBWAVPACK_VERSION_STRING="${LIBWAVPACK_MAJOR}.${LIBWAVPACK_MINOR}.${LIBWAVPACK_MICRO}"

AC_DEFINE_UNQUOTED([LIBWAVPACK_MAJOR], [${LIBWAVPACK_MAJOR}], [libwavpack major version])
AC_DEFINE_UNQUOTED([LIBWAVPACK_MINOR], [${LIBWAVPACK_MINOR}], [libwavpack minor version])
AC_DEFINE_UNQUOTED([LIBWAVPACK_MICRO], [${LIBWAVPACK_MICRO}], [libwavpack micro version])
AC_DEFINE_UNQUOTED([LIBWAVPACK_VERSION_STRING], ["${LIBWAVPACK_VERSION_STRING}"], [libwavpack version string])

dnl set libtool versioning
dnl +1 :  0 : +1   == new interface that does not break old one.
dnl +1 :  0 :  0   == changed/removed an interface. Breaks old apps.
dnl  ? : +1 :  ?   == internal changes that doesn't break anything.
dnl CURRENT : REVISION : AGE
LT_CURRENT=3
LT_REVISION=7
LT_AGE=2

AC_SUBST([LT_CURRENT])
AC_SUBST([LT_REVISION])
AC_SUBST([LT_AGE])

dnl Check for os version
AC_CANONICAL_HOST
AC_DEFINE_UNQUOTED([VERSION_OS], ["${host_os}"], [os version])

dnl Checks for programs.
AC_PROG_CC
AM_PROG_AR
AM_PROG_AS
AC_CHECK_TOOL(RC,[windres],[:])
AC_PROG_INSTALL

LT_INIT([win32-dll disable-static])

dnl Check for large files support
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO

AC_ARG_ENABLE([apps],
  [AS_HELP_STRING([--disable-apps], [build only libwavpack (removes ICONV dependency)])])
AM_CONDITIONAL([ENABLE_APPS], [test "x${enable_apps}" != "xno"])

dnl See whether we prefer libiconv over libc
AC_ARG_ENABLE(libiconv,
  [AS_HELP_STRING([--enable-libiconv], [Prefer iconv() from libiconv, if available, over libc version [default=no]])],, enable_libiconv=no)

AC_MSG_CHECKING([if we are building for a Windows host])
AS_CASE([${host_os}],
  [*mingw*], [windows_host=yes],
  [windows_host=no])
AC_MSG_RESULT([${windows_host}])

AM_CONDITIONAL([WINDOWS_HOST], [test "x${windows_host}" = "xyes"])

dnl Checks for libraries
LT_LIB_M
LIBICONV=
AS_IF([test "x${enable_apps}" != "xno" && test "x${windows_host}" != "xyes"], [
  AC_MSG_CHECKING(for iconv in libc)
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[
   #define LIBICONV_PLUG 1 /* in case libiconv header is in include path */
   #include <stddef.h>
   #include <iconv.h>
  ]],[[
   iconv_open(NULL,NULL);
  ]])], [have_libc_iconv=yes],[have_libc_iconv=no])
  AC_MSG_RESULT($have_libc_iconv)

  AC_MSG_CHECKING(for iconv in libiconv)
  save_LIBS="$LIBS"
  LIBS="$LIBS -liconv"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[
   #include <stddef.h>
   #include <iconv.h>
  ]],[[
   iconv_open(NULL,NULL);
  ]])], [have_libiconv=yes],[have_libiconv=no])
  LIBS="$save_LIBS"
  AC_MSG_RESULT($have_libiconv)

  if test x$have_libc_iconv = xyes || test x$have_libiconv = xyes; then
     if test x$have_libiconv = xyes; then
        if test x$have_libc_iconv != xyes; then
           use_libiconv=yes
        elif test x$enable_libiconv = xyes; then
           use_libiconv=yes
        fi
     fi
     if test x$use_libiconv = xyes; then
        AC_DEFINE(ENABLE_LIBICONV,1,[ ])
        AS_ECHO(["-- Using iconv from libiconv"])
        LIBICONV="-liconv"
     else
        AS_ECHO(["-- Using iconv from libc"])
     fi
  else
    AC_MSG_ERROR([iconv is required for apps, use --disable-apps to build only libwavpack])
  fi
])
AC_SUBST([LIBICONV])

AC_ARG_ENABLE([legacy],
  [AS_HELP_STRING([--enable-legacy], [decode legacy (< 4.0) WavPack files])])
AS_IF([test "x${enable_legacy}" = "xyes"], [
  AC_DEFINE([ENABLE_LEGACY])
])
AM_CONDITIONAL([ENABLE_LEGACY], [test "x${enable_legacy}" = "xyes"])

AC_ARG_ENABLE([dsd],
  [AS_HELP_STRING([--disable-dsd], [disable support for WavPack DSD files])])
AS_IF([test "x${enable_dsd}" != "xno"], [
  AC_DEFINE([ENABLE_DSD])
])
AM_CONDITIONAL([ENABLE_DSD], [test "x${enable_dsd}" != "xno"])

AC_ARG_ENABLE([threads],
  [AS_HELP_STRING([--disable-threads], [disable use of threading in libwavpack])])
AS_IF([test "x${enable_threads}" != "xno"], [
  AC_DEFINE([ENABLE_THREADS])
  AS_IF([test "x${windows_host}" != "xyes"], [
    LIBTHREAD="-lpthread"
  ])
])
AC_SUBST([LIBTHREAD])
AM_CONDITIONAL([ENABLE_THREADS], [test "x${enable_threads}" != "xno"])

AC_ARG_ENABLE([rpath],
  [AS_HELP_STRING([--enable-rpath], [hardcode library path in executables])])
AM_CONDITIONAL([ENABLE_RPATH], [test "x${enable_rpath}" = "xyes"])

AC_ARG_ENABLE([asm],
  [AS_HELP_STRING([--disable-asm], [disable assembly optimizations])], [
  ], [
  enable_asm=check
])
asm_selected=none

AS_IF([test "x${enable_asm}" != "xno"],[
  AC_MSG_CHECKING([if assembly optimizations are available])
  AS_CASE([${host_cpu}],
    [i386|i486|i586|i686|i786], [
      AC_DEFINE([OPT_ASM_X86])
      asm_selected=x86],
    [x86_64], [
      AC_DEFINE([OPT_ASM_X64])
      asm_selected=x64],
    [arm64*], [
      AS_IF([test "x${enable_asm}" = "xyes"], [
        AC_MSG_ERROR([no assembly code for CPU ${host_cpu}])
      ])],
    [arm*], [
      AC_DEFINE([OPT_ASM_ARM])
      asm_selected=arm],
    [AS_IF([test "x${enable_asm}" = "xyes"], [
      AC_MSG_ERROR([no assembly code for CPU ${host_cpu}])])
  ])
  AC_MSG_RESULT([${asm_selected}])
])

AM_CONDITIONAL([ENABLE_X86ASM], [test "x${asm_selected}" = "xx86"])
AM_CONDITIONAL([ENABLE_X64ASM], [test "x${asm_selected}" = "xx64"])
AM_CONDITIONAL([ENABLE_ARMASM], [test "x${asm_selected}" = "xarm"])

dnl Check for __builtin_clz
AC_CACHE_CHECK([for __builtin_clz], [ac_cv_have___builtin_clz],
               AC_LINK_IFELSE([AC_LANG_PROGRAM(,[[return __builtin_clz(1)]])],
               [ac_cv_have___builtin_clz="yes"], [ac_cv_have___builtin_clz="no"]))
AS_IF([test "x${ac_cv_have___builtin_clz}" = "xyes"], [
  AC_DEFINE([HAVE___BUILTIN_CLZ])
])

dnl Check for __builtin_ctz
AC_CACHE_CHECK([for __builtin_ctz], [ac_cv_have___builtin_ctz],
               AC_LINK_IFELSE([AC_LANG_PROGRAM(,[[return __builtin_ctz(1)]])],
               [ac_cv_have___builtin_ctz="yes"], [ac_cv_have___builtin_ctz="no"]))
AS_IF([test "x${ac_cv_have___builtin_ctz}" = "xyes"], [
  AC_DEFINE([HAVE___BUILTIN_CTZ])
])

AS_IF([test "x${ac_cv_c_compiler_gnu}" = "xyes"], [
  CFLAGS="$CFLAGS -Wall"
])

AS_IF([test "x${windows_host}" = "xyes"], [
  AS_IF([test "x$RC" != "x:"],[have_windres=true])
])
AM_CONDITIONAL([HAVE_WINDRES],[test "x$have_windres" = "xtrue"])

AC_CONFIG_LINKS([
  cli/all-tests:cli/all-tests
  cli/fast-tests:cli/fast-tests
])
AC_CONFIG_FILES([
  Makefile
  wavpack.pc
])
AC_OUTPUT
