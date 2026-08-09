#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
if [ -z "$PATH" ]; then
    export PATH="/sbin:/usr/sbin:/bin:/usr/bin"
fi

# Apply BitLSM's additive rocksdb patch to the bundled rocksdb (idempotent).
# The patch ships inside the BitLSM submodule (rocksdb/patches/); MyRocks owns
# *applying* it. Absolute paths: `git -C <rdb>` changes dir, so a relative patch
# path would not resolve. Skip if already applied (reverse-check succeeds).
RDB_ABS="$(pwd)/rocksdb/third_party/rocksdb"
PATCH_ABS="$(pwd)/rocksdb/patches/rocksdb-bitlsm.patch"
if [ -f "$PATCH_ABS" ]; then
  if git -C "$RDB_ABS" apply --reverse --check "$PATCH_ABS" >/dev/null 2>&1; then
    echo "bitlsm rocksdb patch already applied; skipping." >&2
  else
    git -C "$RDB_ABS" apply "$PATCH_ABS" && echo "applied bitlsm rocksdb patch." >&2 \
      || echo "WARN: bitlsm rocksdb patch did not apply cleanly." >&2
  fi
fi

MKFILE=`mktemp`
# create and run a simple makefile
# include rocksdb make file relative to the path of this script
echo "include rocksdb/third_party/rocksdb/src.mk
FOLLY_DIR = ./third-party/folly
all:" > $MKFILE

if [ -z $1 ]; then
  echo "	@echo \"\$(LIB_SOURCES)\"" >> $MKFILE
else
  echo "	@echo \"\$(LIB_SOURCES)\" \"\$(FOLLY_SOURCES)\"" >> $MKFILE
fi
for f in `make --makefile $MKFILE`
do
  echo ../../rocksdb/third_party/rocksdb/$f
done
rm $MKFILE

# BitLSM bitmap-layer sources, compiled into the plugin against the single
# rocksdb. This list is owned by MyRocks (BitLSM stays free of consumer-specific
# files); update it when bumping the BitLSM submodule. roaring comes via
# FetchContent (see CMakeLists); folly-tdigest is compiled from vendored source.
for f in \
  src/include/sabi_factory.cpp \
  src/include/sabi_builder.cpp \
  src/include/sabi_reader.cpp \
  src/include/sabi_table_iterator.cpp \
  src/include/bit_lsm_query.cpp \
  src/include/bit_lsm_iterator.cpp \
  src/include/bit_lsm_shadow_check.cpp \
  src/include/bit_lsm_merging_iterator.cpp \
  src/include/bit_lsm_level_iterator.cpp \
  src/include/bit_lsm_memtable_iterator.cpp \
  src/include/bit_lsm_estimator.cpp \
  third_party/folly-tdigest/src/TDigest.cpp \
  third_party/folly-tdigest/src/DoubleRadixSort.cpp
do
  echo ../../rocksdb/$f
done

# create build_version.cc file. Only create one if it doesn't exists or if it is different
# this is so that we don't rebuild mysqld every time
bv=rocksdb/third_party/rocksdb/util/build_version.cc
build_date=$(date +%F)
pushd rocksdb/third_party/rocksdb>/dev/null
git_sha=$(git rev-parse  HEAD 2>/dev/null)
git_tag=$(git symbolic-ref -q --short HEAD || \
  git describe --tags --exact-match 2>/dev/null)
git_mod=$(git diff-index HEAD --quiet 2>/dev/null; echo $?)
git_date=$(git log -1 --date=format:"%Y-%m-%d %T" --format="%ad" 2>/dev/null)
popd>/dev/null
if [ ! -f $bv ] || [ -z $git_sha ] || [ ! `grep -q $git_sha $bv` ]
then
sed -e s/@GIT_SHA@/$git_sha/ -e s:@GIT_TAG@:"$git_tag":  \
    -e s/@GIT_MOD@/"$git_mod"/ -e s/@BUILD_DATE@/"$build_date"/  \
    -e s/@GIT_DATE@/"$git_date"/ \
    -e s/@ROCKSDB_PLUGIN_BUILTINS@/"$(ROCKSDB_PLUGIN_BUILTINS)"/ \
    -e s/@ROCKSDB_PLUGIN_EXTERNS@/"$(ROCKSDB_PLUGIN_EXTERNS)"/ \
    rocksdb/third_party/rocksdb/util/build_version.cc.in > $bv
fi
