dist_man_MANS = wavpack.1 wvgain.1 wvunpack.1 wvtag.1

if ENABLE_MAN
.xml.1:
	$(XSLTPROC) -nonet http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

SUFFIXES = .1 .xml
endif

EXTRA_DIST = wavpack.xml wvgain.xml wvunpack.xml wvtag.xml

MAINTAINERCLEANFILES = \
	$(man_MANS) \
	Makefile.in

