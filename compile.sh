#!/bin/sh

CUBESCRIPT_PATH="../libcubescript"
OCTASTD_PATH="../octastd"

CXXFLAGS="-g"

c++ globs.cc main.cc $CUBESCRIPT_PATH/cubescript.cc -o main -std=c++11 \
-Wall -Wextra -I. -I$CUBESCRIPT_PATH -I$OCTASTD_PATH -pthread $CXXFLAGS
