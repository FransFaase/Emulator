# Emulator
An i386 emulator for [live-bootstrap](https://github.com/fosslinux/live-bootstrap/).

I am developing an interpreter in order to analyze (and check) the live-bootstrap
contents. I already wrote [a program](https://www.iwriteiam.nl/livebootstrap.html#Parser)
to parse the kaem files and output which files are read and produced. 

## parsing hex0-seed

My first goal was to have the emulator being able to process the `hex0-seed` file
and allow it to reproduce it parsing `hex0_x86.hex0`. Assuming that live-bootstrap
in an adjecent git repository, the following command should work:

```
./Emulator ../live-bootstrap/sysa/stage0-posix/src/bootstrap-seeds/POSIX/x86/hex0-seed ../live-bootstrap/sysa/stage0-posix/src/x86/hex0_x86.hex0 out.bin
diff ../live-bootstrap/sysa/stage0-posix/src/bootstrap-seeds/POSIX/x86/hex0-seed out.bin
```
This goal was achieved in commit [1d5494e2](https://github.com/FransFaase/Emulator/tree/1d5494e262fbfffa3064ee2de3e485b1609f8cd4).
After this changes were made that such that the above command is no longer working

## parsing kaem-optional-seed

My second goal was to have the emulator being able to process the `kaem-optional-seed` file.
This goal was achieved with giving the following command, where the first argument is the
path to the stage0 source directory:
```
./Emulator ../live-bootstrap/sysa/stage0-posix/src/ bootstrap-seeds/POSIX/x86/kaem-optional-seed  kaem.x86
```
This ends with a message about an unknown opcode in the `hex1`.
I also made some changes such that the generated files are placed in the directory
`x86/artifact` where the the emulator is executed.
This was implemented in commit [f88cf442](https://github.com/FransFaase/Emulator/tree/f88cf442fc03696d4dbe78c2b5c678c8818476ff)
The next step will be to be able to process that the `hex1` file.

## parsing hex1

My next goal was to have the emulator being able to process the `hex1` file.
I made some progress and found some interesting resources:
* [X86 Opcode and Instruction Reference Home](http://ref.x86asm.net/geek.html#two-byte)
* [x86 and amd64 instruction reference](https://www.felixcloutier.com/x86/)

After some debugging, for which modified the code to be able to do some debugging,
I managed to implement this in commit [f88cf442](https://github.com/FransFaase/Emulator/tree/f88cf442fc03696d4dbe78c2b5c678c8818476ff)

There appeared to be another bug, which took me a lot of time, and is related to the fact
that sign extension was not used for certain subtract and compare instructions (0x83).
This bug is solved in the commit [5e37d614](https://github.com/FransFaase/Emulator/commit/5e37d614427c412a11375fbfb90e8c4a089b3323).

## Seems we have a working hex2-0

With comment [df17a9eb](https://github.com/FransFaase/Emulator/commit/df17a9eb9716b81b3212472286a8ee404b223871)
it seems we have a working `hex2-0` program. From `M0.hex2` it did produce an `M0` executable,
but it looks like the `M0` contains instruction that have not been implemented yet
in the emulator. Again, I spend substantial time debugging, looking at output
and comparing it with the input files. It cannot be excluded that there are still
bugs in the current Emulator that have resulted in an incorrect working `hex2-0`
program.

## Fixing problems

With commit [9e7eda25](https://github.com/FransFaase/Emulator/commit/9e7eda2556d4d0777943cde7b1cef785ca912ccb)
(and some before), I fixed some problems related to processes effecting each other.
I was not aware that the brk interupt did zero memory and I also did not take care
that the registers where saved properly when switching between processes. The kaem
command line interpreter stores the environment into register, which got corrupted.
It now works until the execution of `cc_x86`, which makes use of some instructions not
yet supported by the emulator.

## cc_x86

In commit [00c51a9d](https://github.com/FransFaase/Emulator/commit/00c51a9de355e09f77474ec6f59f4b2007d37c0c),
I added all the additional instructions that were mentioned in [`cc_x86.M1`](https://github.com/oriansj/stage0-posix-x86/blob/991f9b91b1b99bbb613a87cac619ba32b9555e88/cc_x86.M1). I also had to fix some bug related to the indent getting larger than the
message length of the trace functions. Now it seems to compile `cc_x86` correctly and
it also seems to process `M2-0.c` correctly. But the resulting `M2` program contains
some instructions that are not supported yet. These are mentioned in
[`x86_defs.M1`](https://github.com/oriansj/stage0-posix-x86/blob/991f9b91b1b99bbb613a87cac619ba32b9555e88/x86_defs.M1).
