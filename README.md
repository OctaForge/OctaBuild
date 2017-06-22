# octabuild

OctaBuild is a simple build system that uses the Cubescript language to write
build definitions without depending on a shell to function. It'll also feature
things such as automatic dependency tracking and a large utility library
abstracting away platform differences. Unlike Make, the program flow of
OctaBuild is single threaded, offloading things such as compiler calls
into a thread pool. Therefore, things such as standard output are unaffected
by threading.

It can be used standalone at this point. Once everything is more done, a
library form will be introduced. Several of the features are currently
present via libostd (such as glob matching).

It needs libcubescript to function, which you can fetch at
https://git.octaforge.org/tools/libcubescript.git or at
https://github.com/OctaForge/libcubescript as well as libostd
at https://git.octaforge.org/tools/libostd.git or at
https://github.com/OctaForge/libostd.

## Features

 * A real scripting language
 * Make inspired with a similar style of dependency tracking
 * Parallel builds
 * Glob matching
 * GNU Make style pattern rules

Upcoming features:

 * Automatic dependency tracking
 * Shell independence
 * Platform related utilities
 * Configurations
 * and others

## Usage

Use the provided Makefile to build - adjust the paths to OctaSTD and
libcubescript if necessary. There is a provided example build script
in `example`.

It's also possible to build OctaBuild with OctaBuild. There is a provided
obuild.cfg in the main directory.

The octabuild binary supports the `-h` option to display help.

Keep in mind that the number of jobs is in addition to main thread (unlike
Make, where it specifies the total number of threads).

## License

See `COPYING.md`.
