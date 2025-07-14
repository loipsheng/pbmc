#!/bin/sh

if [ $1 = "d" ]; then
  mkdir -p build-debug
  mkdir -p bin-debug
  cd build-debug
else
  mkdir -p build
  mkdir -p bin
  cd build
fi

if [ $1 = "d" ]; then
  echo "Create Makefile for Debug..."
  cmake -DCMAKE_BUILD_TYPE=Debug .. \
  && make -j2\
  && cp -p pbmc ../bin-debug/ 

elif [ $1 = "r" ]; then
  echo "Create Makefile for Release..."
  cmake -DCMAKE_BUILD_TYPE=Release .. \
  && make -j2\
  && cp -p pbmc ../bin/ 

elif [ $1 = "rs" ]; then
  echo "Create Makefile for Release Static..."
  cmake -DCMAKE_BUILD_TYPE=Release -DPBMC_STATIC_BUILD=on .. \
  && make -j2\
  && cp -p pbmc ../bin/ 

elif [ $1 = "clean" ]; then
  ./clean.sh
fi