# Big-endian test rig

Runs the game code as big-endian MIPS VR4300 under the Unicorn CPU emulator and
compares every frame against the original PC (x86, little-endian) build. Used to
verify the byte-order fix; reusable for any future change.

Linux only (it was run in a Linux container). Needs:

* the libdragon toolchain (`/opt/libdragon/bin/mips64-elf-gcc`, change paths in the scripts if yours differs)
* Unicorn 2.1.3 built from source with `unicorn-2.1.3-epc-badvaddr.patch`
  (exposes the exception address registers so crashes point to a source line):

      git clone -b 2.1.3 https://github.com/unicorn-engine/unicorn
      cd unicorn && git apply ../unicorn-2.1.3-epc-badvaddr.patch
      mkdir build && cd build && cmake .. -G Ninja -DUNICORN_ARCH=mips && ninja

Build the pieces once:

    gcc -O2 -o host host.c -I<unicorn>/include -L<unicorn>/build -lunicorn -Wl,-rpath,<unicorn>/build
    X=mips64-elf; CF="-march=vr4300 -mtune=vr4300 -mabi=o64 -O2"
    $X-gcc $CF -c crt0.S -o crt0.o && $X-gcc $CF -c sys.c -o sys.o

Then run (paths at the top of iterate.sh):

    ./iterate.sh <frames> <script>   # script 0: menus, 1: idle attract demos, 2: new game + random play

It prints the first frame where the big-endian run differs from the PC run,
or the source line of any crash (unaligned access, etc.).

Other tools:

* `PROF=1 ./host <elf> <rom>` then `python3 prof.py <elf>` gives a per-function instruction profile.
* `sheet.py` makes a contact sheet of snapshots (`SNAP_EVERY=N` with the x86 `ref` binary).
* `play_script.h` holds the scripted controller input for script 2: `{frame, length, buttons}`.
