bin_PROGRAMS = wavpack wvunpack wvgain

wavpack_SOURCES = wavpack.c utils.c md5.c
wavpack_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
wavpack_LDFLAGS = -rpath $(libdir)
endif
wavpack_LDADD = $(AM_LDADD) $(top_builddir)/src/.libs/libwavpack.la $(LIBM)

wvunpack_SOURCES = wvunpack.c utils.c md5.c
wvunpack_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
wvunpack_LDFLAGS = -rpath $(libdir)
endif
wvunpack_LDADD = $(AM_LDADD) $(top_builddir)/src/.libs/libwavpack.la $(LIBM)

wvgain_SOURCES = wvgain.c utils.c
wvgain_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
wvgain_LDFLAGS = -rpath $(libdir)
endif
wvgain_LDADD = $(AM_LDADD) $(top_builddir)/src/.libs/libwavpack.la $(LIBM)

if ENABLE_TESTS
bin_PROGRAMS += wvtest
wvtest_SOURCES = wvtest.c md5.c
wvtest_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
wvtest_LDFLAGS = -rpath $(libdir)
endif
wvtest_LDADD = $(AM_LDADD) $(top_builddir)/src/.libs/libwavpack.la $(LIBM) -lpthread
endif

noinst_HEADERS = \
	utils.h \
	md5.h

MAINTAINERCLEANFILES = \
	Makefile.in
