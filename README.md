# octabuild

OctaBuild is a simple build system inspired by Make, but using the Cubescript
language to write build definitions and not depending on a shell to function.
It'll also feature things such as automatic dependency tracking and a large
utility library abstracting away platform differences.

It can be used standalone at this point. Once everything is more done, a library
form will be introduced.

It needs libcubescript to function, which you can fetch at
https://git.octaforge.org/tools/libcubescript.git/ or at
https://github.com/OctaForge/libcubescript.

## Features

 * A real scripting language
 * Similar to Make in many ways
 * Currently manual dependency tracking
 * Parallel builds

Upcoming features:

 * Automatic dependency tracking
 * Shell independence
 * Proper argument handling
 * Platform related utilities

## Usage

Use the provided script to build OctaBuild itself. You will need libcubescript
and octastd in the right paths (you can adjust these paths within `compile.sh`)
in order to compile it. There is a provided example build script in `example`.

The octabuild binary supports the `-h` option to display help.

## License

See `COPYING.md`.