# Elf-xfer

## Overview
This repository contains a collection of utilities for file transfer between a development system and an 1802-based vintage computer system.

## max-xfr
### Name
max-xfr - upload/download files using the MAX protocol
### Synopsis
**max-xfr -s|-r [-v] [-d** *blockdelay* **]**
### Description
Max-xfr transfers files using the MAX protocol, typically with a CDP1802-based vintage computer equipped with either a software or hardware based UART.

Max-xfr reads from stdin when receiving and sends data on stdout when sending. Some form of input and output redirection to a serial port is thus needed. It integrates well with serial communications programs such as minicom or picocom.

If the file name extension is *.hex*, then the file is assumed to be an Intel Hex format file. In the case of receiving a file, the file will be stored in Intel Hex format. When a file is being sent, the data from the file will be transferred using the addresses specified. All other files are transferred as binary images.

The -d option controls the amount of delay between data blocks. This is to allow the 1802 system time to process the block before the next block is sent. This value may be determined empirically for a particular system. Some representative values are:

4MHz system with hardware UART at 57.6kBaud - 150 (default)
4MHz system with software UART at 38.4kBaud - 350
1.8MHz system with software UART at 19.2kBaud - 900

### Options
```
-s  Send a file.
-r  Receive a file.
-v  Verbose: show transfer statistics on stderr output.
-d  Block delay: delay between blocks in microseconds
```
### Usage with minicom
If you want to call this program from minicom(1), start minicom
and go to the Options menu. Select File transfer protocols.  Add
the following lines, for example as protocols I and J.

       I  Ascii    /usr/local/bin/max-xfr -sv -d 350   Y   U   N   Y
       J  Ascii    /usr/local/bin/max-xfr -rv          Y   D   N   Y
## 1802 Code
The mr folder contains the source code for the loadbin subroutine, along with a simple demonstration of its use. The routine is callable via the standard SCRT mechanism. It takes a single parameter in the ra register. This parameter is the offset for the load address. Binary files are always transferred starting at address 0000h, so for binary files ra will specify the start address in memory where the file is to be loaded. In the case of an Intel hex file, the value in ra will be added as an offset to the address specified in the hex file.

The ms folder contains the source code for the savebin subroutine, along with a simple demonstration of its use. The routine is callable via the standard SCRT mechanism. It takes two parameters. Register ra contains the starting address of the memory buffer to be saved, and rc contains the number of bytes to be saved. If the file is saved in Intel hex format, the absolute address of the buffer will appear in the hex file.

The loadbin and savebin functions are included as part of the MAX Monitor program (https://github.com/arhefner/Elf-maxmon/tree/main), which contains standard vector entry points to allow them to be called from user software.
## Elf/OS
The ELfos folder contains a pair of file transfer utilities for Elf/OS.

The **mr** program is used to receive a file from another machine. It takes a single argument, which is the name of the file to be received. This is the Elf/OS name for the file; it does not need to be the same as the name of the file on the source machine.

The **ms** program is used to send a file from Elf/OS to another machine. It takes a single argument, which is the name of the file to be sent.
