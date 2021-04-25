AUTOMAKE_OPTIONS = foreign

if ENABLE_APPS
  MAYBE_APPS = cli
endif

SUBDIRS = src include $(MAYBE_APPS) man

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = wavpack.pc

MAINTAINERCLEANFILES = \
	configure \
	Makefile.in \
	config.guess \
	config.sub \
	ltmain.sh \
	aclocal.m4 \
	depcomp \
	compile \
	missing \
	install-sh

EXTRA_DIST = README.md \
	doc/WavPack5FileFormat.pdf doc/WavPack5LibraryDoc.pdf doc/WavPack5PortingGuide.pdf \
	doc/wavpack_doc.html doc/style.css \
	CMakeLists.txt cmake/CheckCLinkerFlag.cmake cmake/modules/FindIconv.cmake \
	cmake/TestLargeFiles.cmake

