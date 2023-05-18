#!/usr/bin/env bash
set -euo pipefail

export CC='clang'
export CXX='clang++'
compile_time_malloc_conf='background_thread:true,'\
'metadata_thp:auto,'\
'abort_conf:true,'\
'muzzy_decay_ms:0,'\
'zero_realloc:free,'\
'prof_unbias:false,'\
'prof_time_resolution:high'

./autogen.sh \
	--enable-stats \
	--with-private-namespace=jemalloc_ \
	--disable-cache-oblivious \
	--enable-debug \
	--enable-prof \
	--enable-prof-libunwind \
	--with-malloc-conf="$compile_time_malloc_conf" \
	--enable-readlinkat \
	--enable-opt-safety-checks \
	--enable-uaf-detection \
	--enable-force-getenv

bear -- make -s -j $(nproc)
# We end up with lots of duplicate entries in the compilation database, one for
# each output file type (e.g. .o, .d, .sym, etc.). The must be exactly one
# entry for each file in the compilation database in order for
# cross-translation-unit analysis to work, so we deduplicate the database here.
jq '[.[] | select(.output | test("/[^./]*\\.o$"))]' compile_commands.json > compile_commands.json.tmp
mv compile_commands.json.tmp compile_commands.json

CC_ANALYZERS_FROM_PATH=1 CodeChecker analyze compile_commands.json --jobs $(nproc) \
	--ctu --compile-uniqueing strict --output static_analysis_raw_results \
	--analyzers clang-tidy clangsa

# We're echoing a value because we want to indicate whether or not any errors
# were found, but we always want the script to have a successful exit code so
# that we actually reach the step in the GitHub action where we upload the results.
if CodeChecker parse --export html --output "$1" static_analysis_raw_results
then
	echo "HAS_STATIC_ANALYSIS_RESULTS=0" >> "$2"
else
	echo "HAS_STATIC_ANALYSIS_RESULTS=1" >> "$2"
fi
