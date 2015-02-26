lib_LTLIBRARIES = libwavpack.la

libwavpack_la_SOURCES = \
	common_utils.c \
	decorr_utils.c \
	entropy_utils.c \
	extra1.c \
	extra2.c \
	open_utils.c \
	pack.c \
	pack_dns.c \
	pack_floats.c \
	pack_utils.c \
	read_words.c \
	tags.c \
	tag_utils.c \
	unpack3.c \
	unpack3_open.c \
	unpack3_seek.c \
	unpack.c \
	unpack_floats.c \
	unpack_seek.c \
	unpack_utils.c \
	write_words.c

if ENABLE_X86ASM
libwavpack_la_SOURCES += pack_x86.s unpack_x86.s
endif

if ENABLE_X64ASM
libwavpack_la_SOURCES += pack_x64.s unpack_x64.s
endif

if ENABLE_ARMASM
libwavpack_la_SOURCES += unpack_armv7.s
endif

noinst_HEADERS = \
	decorr_tables.h \
	unpack3.h \
	wavpack_local.h \
	wavpack_version.h

libwavpack_la_CFLAGS = $(AM_CFLAGS) $(ICONV_CFLAGS)
libwavpack_la_LIBADD = $(AM_LDADD) $(LIBM) $(ICONV_LIBS)
libwavpack_la_LDFLAGS = -version-info $(LT_CURRENT):$(LT_REVISION):$(LT_AGE) -export-symbols-regex '^Wavpack.*$$' -no-undefined

MAINTAINERCLEANFILES = \
	Makefile.in
