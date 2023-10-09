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