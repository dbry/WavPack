gcc -DSTAND_ALONE_LENGTH=1000000 -g3 -fsanitize=address -fno-omit-frame-pointer -I../include fuzzer.cc ../src/.libs/libwavpack.a -lm -o fuzzer
