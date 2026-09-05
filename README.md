# Elf-xfer

## Overview
This repository contains a collection of utilities for file transfer between a development system and an 1802-based vintage computer system.

## max-xfr
### Name
max-xfr - upload/download files using the MAX protocol, matching ELF-DOS's `MR`/`MS` commands
### Synopsis
**max-xfr -s [-v] [-d** *delay* **] <file> [file...]**
**max-xfr -r [-v] [-d** *delay* **] [<destination>]**
### Description
Max-xfr transfers one or more files in a single batch, using the MAX protocol, typically with a CDP1802-based vintage computer (ELF-DOS's `MR`/`MS`) equipped with either a software (bit-banged) or hardware UART.

Max-xfr reads from stdin when receiving and writes to stdout when sending. Some form of input/output redirection to a serial port is needed. It integrates well with serial communications programs such as minicom or picocom.

`-s` sends every file named on the command line, in order; wildcards are expanded by the shell before max-xfr ever sees them. `-r` receives whatever the sender offers: a destination that's an existing directory saves each file under its own name into that directory (or the current directory, if no destination is given at all); any other destination name saves only the first file offered, under that exact name, draining and discarding any further files so the session still ends cleanly.

Every file is transferred as a raw binary image -- there is no Intel Hex support in this tool (an earlier version of max-xfr supported `.hex` files directly; that mode was removed since it never made sense in the same address-agnostic, file-oriented protocol `MR`/`MS` actually use). A separate utility will eventually handle BIOS-level hex/binary memory-image transfers instead.

The `-d` option adds a delay (in microseconds) before each byte sent, and (as of this writing) before each ack byte written back while receiving. This exists because a software (bit-banged) UART on the ELF-DOS side has no buffering at all: it has to already be polling for a byte's start bit at the instant that bit begins, or the byte is lost outright -- not queued, not retried. `-d` gives the far end (or, for an ack, the *near* end -- max-xfr itself) time to get back to its own listen call before the next byte goes out. The right value is somewhat empirical and depends on the actual baud rate and whether the ELF-DOS side is using its hardware UART (`MR`/`MS -u`) or the bit-banged one (`-b`); a hardware UART's own small receive buffer tolerates a much smaller (or zero) delay than a bit-banged one does. Start with something in the 500-1000 microsecond range for a bit-banged link and adjust from there; a real hardware UART link often needs none at all.

### Options
```
-s  Send one or more files (batch mode).
-r  Receive whatever the sender offers (batch mode).
-v  Verbose: show transfer statistics and per-file progress on stderr.
-d  Delay in microseconds before each byte sent, and before each ack
    byte while receiving.
```
### Usage with minicom
If you want to call this program from minicom(1), start minicom
and go to the Options menu. Select File transfer protocols.  Add
the following lines, for example as protocols I and J.

       I  Ascii    /usr/local/bin/max-xfr -sv -d 1000   Y   U   N   Y
       J  Ascii    /usr/local/bin/max-xfr -rv -d 1000   Y   D   N   Y
## 1802 Code
The mr folder contains the source code for the loadbin subroutine, along with a simple demonstration of its use. The routine is callable via the standard SCRT mechanism. It takes a single parameter in the ra register. This parameter is the offset for the load address. Binary files are always transferred starting at address 0000h, so for binary files ra will specify the start address in memory where the file is to be loaded. In the case of an Intel hex file, the value in ra will be added as an offset to the address specified in the hex file.

The ms folder contains the source code for the savebin subroutine, along with a simple demonstration of its use. The routine is callable via the standard SCRT mechanism. It takes two parameters. Register ra contains the starting address of the memory buffer to be saved, and rc contains the number of bytes to be saved. If the file is saved in Intel hex format, the absolute address of the buffer will appear in the hex file.

The loadbin and savebin functions are included as part of the MAX Monitor program (https://github.com/arhefner/Elf-maxmon/tree/main), which contains standard vector entry points to allow them to be called from user software.
## Elf/OS
The ELfos folder contains a pair of file transfer utilities for Elf/OS.

The **mr** program is used to receive a file from another machine. It takes a single argument, which is the name of the file to be received. This is the Elf/OS name for the file; it does not need to be the same as the name of the file on the source machine.

The **ms** program is used to send a file from Elf/OS to another machine. It takes a single argument, which is the name of the file to be sent.
