# LILT

A musical grid for small computers. Place notes on a sixteen by eight grid, let them loop and shape the sound with echo, space, pace and wobble across nine generated voices.

One C file, SDL2 only. No samples, fonts or other assets.

Download: https://deadquiet42.github.io/lilt/

## Build

```
gcc -O2 -s lilt.c -o lilt $(sdl2-config --cflags --libs) -lm -lpthread
```

The PortMaster release is cross compiled with Zig 0.14.1 for aarch64, glibc 2.17:

```
zig cc -target aarch64-linux-gnu.2.17 -O2 -s -I <SDL2 include> -D_REENTRANT \
    lilt.c -o lilt -L <aarch64 libSDL2.so dir> -lSDL2 -lm -lpthread
```

## Run

```
./lilt [save directory]
```

## Licence

Freeware. See [LICENSE](LICENSE).
