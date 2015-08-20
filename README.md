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

Upcoming features:

 * Automatic dependency tracking
 * Parallel builds
 * Shell independence
 * Proper argument handling
 * Platform related utilities