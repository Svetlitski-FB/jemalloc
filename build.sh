#!/usr/bin/env bash

compile_time_malloc_conf='background_thread:true,'\
'metadata_thp:auto,'\
'abort_conf:true,'\
'muzzy_decay_ms:0,'\
'zero_realloc:free,'\
'prof_unbias:false,'\
'prof_time_resolution:high'

export CC='clang'
export CXX='clang++'
extra_flags='-march=native -mtune=native'
export EXTRA_CFLAGS="$extra_flags"
export EXTRA_CXXFLAGS="$extra_flags"

if command -v lld >/dev/null 2>&1
then
	export LDFLAGS='-fuse-ld=lld'
fi

./autogen.sh \
	--enable-stats \
	--with-private-namespace=jemalloc_ \
	--disable-cache-oblivious \
	--enable-prof \
	--enable-prof-libunwind \
	--with-malloc-conf="$compile_time_malloc_conf" \
	--enable-readlinkat \
	--enable-opt-safety-checks \
	--enable-uaf-detection \
	--enable-force-getenv \
	--disable-dss

make -s -j "$(nproc)" all test/unit/page_allocator_replay
