# octabuild

OctaBuild is a simple build system inspired by Make, but using the
Cubescript language to write build definitions and not depending on a
shell to function. It'll also feature things such as automatic dependency
tracking and a large utility library abstracting away platform differences.

It can be used standalone at this point. Once everything is more done, a
library form will be introduced.

It needs libcubescript to function, which you can fetch at
https://git.octaforge.org/tools/libcubescript.git or at
https://github.com/OctaForge/libcubescript as well as OctaSTD
at https://git.octaforge.org/tools/octastd.git or at
https://github.com/OctaForge/OctaSTD.

## Features

 * A real scripting language
 * Similar to Make in many ways
 * Currently manual dependency tracking
 * Parallel builds
 * Glob matching
 * GNU Make style pattern rules

Upcoming features:

 * Automatic dependency tracking
 * Shell independence
 * Proper argument handling
 * Platform related utilities

## Usage

Use the provided Makefile to build - adjust the paths to OctaSTD and
libcubescript if necessary. There is a provided example build script
in `example`.

The octabuild binary supports the `-h` option to display help.

## License

See `COPYING.md`.
