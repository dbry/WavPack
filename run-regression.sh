make clean
./configure --disable-apps CFLAGS="-g3 -fsanitize=address -fno-omit-frame-pointer"
make
gcc -DSTAND_ALONE_LENGTH=1000000 -g3 -fsanitize=address -fno-omit-frame-pointer -Iinclude fuzzing/fuzzer.cc src/.libs/libwavpack.a -lm -o fuzzer
./fuzzer fuzzing/regression/*.wv
make clean
rm fuzzer

