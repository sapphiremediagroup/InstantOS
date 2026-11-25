# The desktop system's compiler, checked from inside it: login runs this as
# /autorun.sh (with bash) once the compositor is up, on the dev disk
# (tools/make-dev-disk.sh). Everything goes to /autorun.log; the DEVSMOKE_*
# markers also go to /dev/console, i.e. the serial log
# (tools/run-dev-smoke-win.sh checks them).
exec > /autorun.log 2>&1
say() { echo "$*"; echo "$*" > /dev/console; }
export PATH=/usr/bin:/bin HOME=/home TMPDIR=/tmp
say DEVSMOKE_BEGIN
say "DEVSMOKE_GCC $(gcc --version | head -1)"
mkdir -p /tmp/devsmoke && cd /tmp/devsmoke || { say DEVSMOKE_FAIL no-tmp; say DEVSMOKE_DONE; exit 1; }

# C, with libm.
if gcc -O2 -Wall /examples/hello.c -o hello -lm && ./hello > c.out && grep -q "sqrt(2) = 1.414214" c.out; then
  say DEVSMOKE_C_OK
else
  say DEVSMOKE_C_FAIL; cat c.out
fi

# C++: iostream, std::string, std::vector, an exception thrown and caught
# (libstdc++.so.6 and libgcc_s.so.1 from /usr/lib).
if g++ -O2 /examples/hello.cpp -o hellopp && ./hellopp > cxx.out && grep -q "hello from g++ on InstantOS" cxx.out; then
  say DEVSMOKE_CXX_OK
else
  say DEVSMOKE_CXX_FAIL; cat cxx.out
fi

# make with two sources and a header; the program exits 7.
mkdir -p proj && cd proj
printf '#pragma once\nint add(int a, int b);\n' > util.h
printf '#include "util.h"\nint add(int a, int b) { return a + b; }\n' > util.c
printf '#include "util.h"\n#include <stdio.h>\nint main(void) { int r = add(3, 4); printf("3 + 4 = %%d\\n", r); return r; }\n' > main.c
printf 'app: main.o util.o\n\t$(CC) -o $@ $^\n%%.o: %%.c util.h\n\t$(CC) -O2 -c $< -o $@\n' > Makefile
make CC=gcc && ./app; rc=$?
if [ "$rc" = 7 ]; then say DEVSMOKE_MAKE_OK; else say "DEVSMOKE_MAKE_FAIL rc=$rc"; fi
cd /tmp/devsmoke

ls -l /tmp/devsmoke
say DEVSMOKE_DONE
