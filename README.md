# LILT

A musical grid for small computers. Place notes on a sixteen by eight grid and let them loop, then shape the sound with echo, space, pace and wobble across nine generated voices.

LILT is a single C file. It draws its own interface and generates all of its sound in real time through SDL2, with no samples, fonts or other assets.

Download: https://deadquiet42.github.io/lilt/
PortMaster: available as a port for aarch64 handhelds.

## Building

LILT needs only SDL2 and the C standard library.

### Native build

On an aarch64 device or chroot with the SDL2 development files installed:

```
gcc -O2 -s lilt.c -o lilt $(sdl2-config --cflags --libs) -lm -lpthread
```

### Cross build (as used for the PortMaster release)

Cross compiled with Zig 0.14.1, targeting glibc 2.17, against the SDL2 2.30.9 headers:

```
zig cc -target aarch64-linux-gnu.2.17 -O2 -s -I <SDL2 include dir> -D_REENTRANT \
    lilt.c -o lilt -L <dir containing an aarch64 libSDL2.so> -lSDL2 -lm -lpthread
```

The library directory only has to satisfy the linker; any aarch64 `libSDL2.so` works, including a stub. At runtime the binary uses the system's `libSDL2-2.0.so.0`, `libm`, `libpthread` and `libc` (glibc 2.17 or later).

## Running

```
./lilt [save directory]
```

The pattern and settings are saved to `lilt.state` in the given directory.

## Licence

LILT is freeware. See [LICENSE](LICENSE).
