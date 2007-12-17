AUTOMAKE_OPTIONS = foreign

lib_LTLIBRARIES = libwavpack.la

libdir = @XMMS_INPUT_PLUGIN_DIR@

INCLUDES = @GLIB_CFLAGS@ @XMMS_CFLAGS@ @GTK_CFLAGS@ -DVERSION=\"@VERSION@\"
libwavpack_la_LIBADD = @PTHREAD_LIBS@ @GLIB_LIBS@ @XMMS_LIBS@ @GTK_LIBS@

libwavpack_la_SOURCES = equalizer.cpp libwavpack.cpp ui.cpp
libwavpack_la_CXXFLAGS = -fPIC -DTAGS
libwavpack_la_LDFLAGS = -avoid-version


