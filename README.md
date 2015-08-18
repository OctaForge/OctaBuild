# octabuild

OctaBuild is a simple build system primarily intended to handle building of
OctaForge binary modules.

It can also be used standalone. It consists of a library and an executable
that is basically a frontend for the library. [not yet done]

It's similar in philosophy to tools such as Make.

It needs libcubescript to function, which you can fetch at
https://git.octaforge.org/tools/libcubescript.git/ or at
https://github.com/OctaForge/libcubescript.

## Features

 * A real scripting language
 * Similar to Make in many ways
 * Currently manual dependency tracking

Upcoming features:

 * Automatic dependency tracking
 * Parallel builds
 * Shell independence
 * Proper argument handling
 * Platform related utilities