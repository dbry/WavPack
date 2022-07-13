#!/bin/sh

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`
cd $srcdir

test -f "config.rpath" || cp /usr/share/gettext/config.rpath . 2>/dev/null || touch config.rpath || exit 1

autoreconf -v --install || exit 1
cd $ORIGDIR || exit $?

if test -z "$NOCONFIGURE"; then
    $srcdir/configure --enable-maintainer-mode --enable-man "$@"
fi
