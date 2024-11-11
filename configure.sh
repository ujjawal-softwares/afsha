#! /bin/sh

cmake -S . -DSDL_SHARED=OFF -DSDL_STATIC=ON -B ./out/build
