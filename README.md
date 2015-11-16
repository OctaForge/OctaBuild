# octabuild

OctaBuild is a simple build system inspired by Make, but using the Cubescript
language to write build definitions and not depending on a shell to function.
It'll also feature things such as automatic dependency tracking and a large
utility library abstracting away platform differences. Unlike Make, the program
flow of OctaBuild is single threaded, offloading things such as compiler calls
into a thread pool. Therefore, things such as standard output are unaffected
by threading.

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
 * Rule pre-callbacks
 * Option sets (configurations)

## Usage

Use the provided Makefile to build - adjust the paths to OctaSTD and
libcubescript if necessary. There is a provided example build script
in `example`.

It's also possible to build OctaBuild with OctaBuild. There is a provided
obuild.cfg in the main directory.

The octabuild binary supports the `-h` option to display help.

## License

See `COPYING.md`.
