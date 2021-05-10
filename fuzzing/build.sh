#!/bin/bash -eu
# Copyright 2019 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

# build project
# e.g.
./autogen.sh --disable-apps --disable-shared --enable-static
CFLAGS="$CFLAGS -fno-sanitize=signed-integer-overflow" ./configure --disable-apps --disable-shared --enable-static
CFLAGS="$CFLAGS -fno-sanitize=signed-integer-overflow" make

# build fuzzers
# e.g.
$CXX $CXXFLAGS -std=c++11 -I$SRC/wavpack/include -I$SRC/wavpack/cli \
     $SRC/wavpack/fuzzing/fuzzer.cc -o $OUT/fuzzer \
     $LIB_FUZZING_ENGINE $SRC/wavpack/src/.libs/libwavpack.a

# add seed corpus
cp $SRC/wavpack/fuzzing/*_seed_corpus.zip $OUT/

# add dictionary
cp $SRC/wavpack/fuzzing/*.dict $SRC/wavpack/fuzzing/*.options $OUT/
