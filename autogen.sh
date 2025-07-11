#!/bin/sh

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`
cd $srcdir

mkdir -p m4 || exit 1

"${AUTORECONF:-autoreconf}" -v --install || exit 1
cd $ORIGDIR || exit $?

if test -z "$NOCONFIGURE"; then
    $srcdir/configure --enable-maintainer-mode "$@"
fi
