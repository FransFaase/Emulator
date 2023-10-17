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
This goal was achieved in commit [1d5494e2](commit/1d5494e262fbfffa3064ee2de3e485b1609f8cd4).
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
The next step will be to be able to process that the `hex1` file.
