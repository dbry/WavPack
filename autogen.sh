#!/bin/sh

DIE=0
PACKAGE=wavpack

echo "Generating configuration files for $PACKAGE, please wait..."

(autoconf --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have autoconf installed to compile $PACKAGE."
	echo "Download the appropriate package for your distribution,"
	echo "or get the source tarball at ftp://ftp.gnu.org/pub/gnu/"
	DIE=1
}

for LIBTOOLIZE in libtoolize glibtoolize nope; do
	$LIBTOOLIZE --version > /dev/null 2>&1 && break
done
if test x$LIBTOOLIZE = xnope; then
	echo
	echo "You must have libtool installed to compile $PACKAGE."
	echo "Download the appropriate package for your distribution,"
	echo "or get the source tarball at ftp://ftp.gnu.org/pub/gnu/"
	DIE=1
fi

(automake --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have automake installed to compile $PACKAGE."
	echo "Download the appropriate package for your distribution,"
	echo "or get the source tarball at ftp://ftp.gnu.org/pub/gnu/"
	DIE=1
}

[ $DIE -eq 1 ] && exit 1

touch NEWS README AUTHORS ChangeLog
aclocal
$LIBTOOLIZE --copy --force
automake --copy --add-missing --force
autoconf --force
