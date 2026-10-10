#!/bin/bash
F="-I cgdoom -I src-sdl2 -DCG_EMULATOR -DCGD_BENCH $(pkg-config sdl2 --cflags) -O1 -g -w -fno-strict-aliasing"
fail=0
for f in cgdoom/*.c src-sdl2/*.c; do o=obj/$(basename $f .c).o; clang $F -c $f -o $o 2> obj/$(basename $f .c).err || { fail=1; echo "FAIL $f"; head -8 obj/$(basename $f .c).err; }; done
[ $fail = 0 ] && clang obj/*.o $(pkg-config sdl2 --libs) -o cgdbench && echo BUILD-OK
