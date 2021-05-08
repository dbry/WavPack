#!/bin/bash

# using clang unless overridden by first parameter

cc=${1:-"clang"}

# delete any previously build checkers and copy the fuzzer to C

rm check-address check-memory check-undefined
cp fuzzing/fuzzer.cc fuzzing/fuzzer.c

# build the address sanitizer first

if ./autogen.sh CC="$cc" --enable-static --disable-apps CFLAGS="-g3 -fsanitize=address -fno-omit-frame-pointer" && make clean && make && "$cc" -DSTAND_ALONE_LENGTH=1000000 -g3 -fsanitize=address -fno-omit-frame-pointer -Iinclude fuzzing/fuzzer.c src/.libs/libwavpack.a -lm -o check-address
then
    echo address sanitizer build was successful
else
    echo address sanitizer build failed
    exit 1
fi

# then build the memory sanitizer

if make clean && ./configure CC="$cc" --enable-static --disable-apps CFLAGS="-g3 -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer -fPIE -pie" && make && "$cc" -DSTAND_ALONE_LENGTH=1000000 -g3 -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer -fPIE -pie -Iinclude fuzzing/fuzzer.c src/.libs/libwavpack.a -lm -o check-memory
then
    echo memory sanitizer build was successful
else
    echo memory sanitizer build failed
    exit 1
fi

# finally build the undefined sanitizer

if make clean && ./configure CC="$cc" --enable-static --disable-apps CFLAGS="-g3 -fsanitize=undefined -fno-sanitize=signed-integer-overflow -fno-sanitize-recover -fno-omit-frame-pointer" && make && "$cc" -DSTAND_ALONE_LENGTH=1000000 -g3 -fsanitize=undefined -fno-sanitize=signed-integer-overflow -fno-sanitize-recover -fno-omit-frame-pointer -Iinclude fuzzing/fuzzer.c src/.libs/libwavpack.a -lm -o check-undefined
then
    echo undefined sanitizer build was successful
else
    echo undefined sanitizer build failed
    exit 1
fi

echo all three regression testers built without errors
rm fuzzing/fuzzer.c
make clean
