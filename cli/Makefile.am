bin_PROGRAMS = wavpack-stream wvunpack-stream

wavpack_stream_SOURCES = wavpack.c riff.c wave64.c caff.c dsdiff.c dsf.c utils.c md5.c
if WINDOWS_HOST
wavpack_stream_SOURCES += win32_unicode_support.c
endif
wavpack_stream_CFLAGS = $(AM_CFLAGS) $(ICONV_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
wavpack_stream_LDFLAGS = -rpath $(libdir)
endif
wavpack_stream_LDADD = $(AM_LDADD) $(top_builddir)/src/.libs/libwavpack-stream.la $(LIBM) $(ICONV_LIBS)

wvunpack_stream_SOURCES = wvunpack.c riff.c wave64.c caff.c dsdiff.c dsf.c utils.c md5.c
if WINDOWS_HOST
wvunpack_stream_SOURCES += win32_unicode_support.c
endif
wvunpack_stream_CFLAGS = $(AM_CFLAGS) $(ICONV_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
wvunpack_stream_LDFLAGS = -rpath $(libdir)
endif
wvunpack_stream_LDADD = $(AM_LDADD) $(top_builddir)/src/.libs/libwavpack-stream.la $(LIBM) $(ICONV_LIBS)

if ENABLE_TESTS
bin_PROGRAMS += wvtest-stream
wvtest_stream_SOURCES = wvtest.c md5.c
wvtest_stream_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
wvtest_stream_LDFLAGS = -rpath $(libdir)
endif
wvtest_stream_LDADD = $(AM_LDADD) $(top_builddir)/src/.libs/libwavpack-stream.la $(LIBM) -lpthread
endif

noinst_HEADERS = \
	win32_unicode_support.h \
	utils.h \
	md5.h

MAINTAINERCLEANFILES = \
	Makefile.in
