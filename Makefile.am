pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = wavpack.pc

dist_doc_DATA = README.md
dist_pdf_DATA = \
	doc/WavPack5FileFormat.pdf \
	doc/WavPack5LibraryDoc.pdf \
	doc/WavPack5PortingGuide.pdf
dist_html_DATA = \
	doc/wavpack_doc.html \
	doc/style.css

EXTRA_DIST = \
	CMakeLists.txt \
	cmake/CheckCLinkerFlag.cmake \
	cmake/modules/FindIconv.cmake \
	cmake/TestLargeFiles.cmake \
	wavpackdll/resource.h \
	wavpackdll/wavpackdll.rc

MAINTAINERCLEANFILES = \
	aclocal.m4 \
	ar-lib \
	compile \
	config.guess \
	config.rpath \
	config.sub \
	configure \
	depcomp \
	install-sh \
	ltmain.sh \
	Makefile.in \
	missing

########
# cli/ #
########

if ENABLE_APPS
bin_PROGRAMS = cli/wavpack cli/wvunpack cli/wvgain cli/wvtag
endif

cli_wavpack_SOURCES = cli/wavpack.c cli/riff.c cli/wave64.c cli/caff.c cli/dsdiff.c cli/dsf.c cli/aiff.c cli/utils.c cli/md5.c cli/import_id3.c
if WINDOWS_HOST
cli_wavpack_SOURCES += cli/win32_unicode_support.c
endif
cli_wavpack_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
cli_wavpack_LDFLAGS = -rpath $(libdir)
endif
cli_wavpack_LDADD = $(AM_LDADD) src/libwavpack.la $(LIBM) $(LIBICONV)

cli_wvunpack_SOURCES = cli/wvunpack.c cli/riff_write.c cli/wave64_write.c cli/caff_write.c cli/dsdiff_write.c cli/aiff_write.c cli/dsf_write.c cli/utils.c cli/md5.c
if WINDOWS_HOST
cli_wvunpack_SOURCES += cli/win32_unicode_support.c
endif
cli_wvunpack_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
cli_wvunpack_LDFLAGS = -rpath $(libdir)
endif
cli_wvunpack_LDADD = $(AM_LDADD) src/libwavpack.la $(LIBM) $(LIBICONV)

cli_wvgain_SOURCES = cli/wvgain.c cli/utils.c
if WINDOWS_HOST
cli_wvgain_SOURCES += cli/win32_unicode_support.c
endif
cli_wvgain_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
cli_wvgain_LDFLAGS = -rpath $(libdir)
endif
cli_wvgain_LDADD = $(AM_LDADD) src/libwavpack.la $(LIBM)

cli_wvtag_SOURCES = cli/wvtag.c cli/utils.c cli/import_id3.c
if WINDOWS_HOST
cli_wvtag_SOURCES += cli/win32_unicode_support.c
endif
cli_wvtag_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
cli_wvtag_LDFLAGS = -rpath $(libdir)
endif
cli_wvtag_LDADD = $(AM_LDADD) src/libwavpack.la $(LIBM) $(LIBICONV)

check_PROGRAMS = cli/wvtest
cli_wvtest_SOURCES = cli/wvtest.c cli/md5.c
cli_wvtest_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
if ENABLE_RPATH
cli_wvtest_LDFLAGS = -rpath $(libdir)
endif
cli_wvtest_LDADD = $(AM_LDADD) src/libwavpack.la $(LIBM) -lpthread

TESTS = cli/fast-tests
TESTS_ENVIRONMENT = $(SHELL)

noinst_HEADERS = \
	cli/win32_unicode_support.h \
	cli/utils.h \
	cli/md5.h

############
# include/ #
############
pkginclude_HEADERS = include/wavpack.h

########
# man/ #
########
if ENABLE_MAN
.xml.1:
	$(XSLTPROC) -nonet -o man/ http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

SUFFIXES = .1 .xml
endif

dist_man_MANS = \
	man/wavpack.1 \
	man/wvgain.1 \
	man/wvunpack.1 \
	man/wvtag.1

EXTRA_DIST += \
	man/wavpack.xml \
	man/wvgain.xml \
	man/wvunpack.xml \
	man/wvtag.xml

MAINTAINERCLEANFILES += $(man_MANS)

########
# src/ #
########
lib_LTLIBRARIES = src/libwavpack.la

src_libwavpack_la_SOURCES = \
	src/common_utils.c \
	src/decorr_utils.c \
	src/entropy_utils.c \
	src/extra1.c \
	src/extra2.c \
	src/open_utils.c \
	src/open_filename.c \
	src/open_legacy.c \
	src/open_raw.c \
	src/pack.c \
	src/pack_dns.c \
	src/pack_floats.c \
	src/pack_utils.c \
	src/read_words.c \
	src/tags.c \
	src/tag_utils.c \
	src/unpack.c \
	src/unpack_floats.c \
	src/unpack_seek.c \
	src/unpack_utils.c \
	src/write_words.c \
	src/decorr_tables.h \
	src/unpack3.h \
	src/wavpack_local.h \
	src/wavpack_version.h

if ENABLE_LEGACY
src_libwavpack_la_SOURCES += src/unpack3.c src/unpack3_open.c src/unpack3_seek.c
endif

if ENABLE_DSD
src_libwavpack_la_SOURCES += src/pack_dsd.c src/unpack_dsd.c
endif

if ENABLE_X86ASM
src_libwavpack_la_SOURCES += src/pack_x86.S src/unpack_x86.S
endif

if ENABLE_X64ASM
src_libwavpack_la_SOURCES += src/pack_x64.S src/unpack_x64.S
endif

if ENABLE_ARMASM
src_libwavpack_la_SOURCES += src/unpack_armv7.S
endif

src_libwavpack_la_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
src_libwavpack_la_LIBADD = $(AM_LDADD) $(LIBM)
src_libwavpack_la_LDFLAGS = -version-info $(LT_CURRENT):$(LT_REVISION):$(LT_AGE) -export-symbols-regex '^Wavpack.*$$' -no-undefined
