#!/bin/bash

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=$HERE/..

mkdir -p $ROOT/src
cd $ROOT/src

if [ ! -e v-hacd ]; then
  git clone https://github.com/kmammou/v-hacd.git
fi

if [ ! -e $ROOT/.anaconda3/bin/testVHACD ]; then
  cd $ROOT/src/v-hacd
  # https://github.com/wkentaro/morefusion/issues/102#issue-1270898773
  # This is due to changes in the v-hacd repo.
  # You can solve the problem by switching to an older version. In the morefusion/src/v-hacd directory, do:
  git checkout 1a49edf29c69039df15286181f2f27e17ceb9aef
  mkdir -p build
  cd build
  cmake ../src
  make -j

  cd $ROOT/.anaconda3/bin
  ln -s ../../src/v-hacd/build/test/testVHACD
fi
