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

Every file is transferred as a raw binary image -- there is no Intel Hex support in this tool (an earlier version of max-xfr supported `.hex` files directly; that mode was removed since it never made sense in the same address-agnostic, file-oriented protocol `MR`/`MS` actually use). BIOS-level hex/binary memory-image transfers are handled by `mem-xfr` instead; see below.

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
## mem-xfr
### Name
mem-xfr - transfer a binary or Intel hex memory image to/from the MAX Monitor's `loadbin`/`savebin` routines
### Synopsis
**mem-xfr -s [-v] [-x] [-d** *delay* **] [-a** *addr* **] [-o** *offset* **] [-l** *length* **]** *file*
**mem-xfr -r [-v] [-x] [-d** *delay* **] [-a** *addr* **] [-o** *offset* **] [-l** *length* **]** *file*
### Description
Mem-xfr transfers a raw memory image to or from a CDP1802-based system running the MAX Monitor, talking to its `loadbin` and `savebin` routines (the monitor's own `L` and `S` commands, or the `m_loadbin`/`m_savebin` vectors from user software). Like max-xfr, it reads from stdin and writes to stdout, so some form of redirection to a serial port is needed, and it works well when called from minicom or picocom.

Mem-xfr and max-xfr do different jobs and speak different protocols; the split is deliberate rather than historical. **Max-xfr moves named files:** its unit is "one file = one name + one contiguous byte stream", it has no concept of an address, and it can stream a file of any size. **Mem-xfr moves a memory image:** its unit is "these bytes belong at this 1802 address", there are no filenames on the wire at all, and the whole address space is 64K. Mem-xfr therefore holds the image in memory, which is also what makes sparse Intel hex files work: it tracks which locations a file actually mentioned, so gaps between hex records stay gaps instead of being transmitted as runs of zeros.

`mem-xfr -s` sends all or part of a file to `loadbin`. `mem-xfr -r` receives a block of memory from `savebin`. `-x` says the local file is Intel hex rather than raw binary; it changes only how the file is read or written, never what goes over the wire.

On input, `-x` reads data (`00`) and end-of-file (`01`) records, and skips the start-address records (`03` and `05`) that asm/02 emits to record a program's entry point -- `-v` reports that entry point rather than loading it, since it is where the monitor's `G` command would begin. Extended-address records (`02` and `04`) are accepted only when their base resolves to zero: the 1802 has no address space beyond 64K for a nonzero base to refer to. Any other record type is an error. On output, mem-xfr writes only data and end-of-file records.

Gaps between records are preserved rather than filled. Sending `ledclock.hex`, for instance, transmits its 1174 bytes as nine separate blocks and leaves the 817 bytes of gap between them untouched on the 1802, so whatever already occupied those addresses survives the load.

#### Addresses, offsets and lengths
The three address-related options mean slightly different things in each direction, because the two routines divide the work differently.

When **sending**, mem-xfr chooses the addresses. For a binary file, `-o` is the offset within the file to start reading at, `-l` is how many bytes to send, and `-a` is the 1802 address the first of those bytes loads at. For an Intel hex file the addresses come from the records themselves, so `-a` and `-l` instead select which part of the image to send, and `-o` is added to every record's address as a relocation.

Note that `loadbin` adds its own `ra` parameter to whatever address arrives on the wire, so with the monitor's `L` command the final load address is `-a` plus the offset typed after `L`. Using `-a` alone (and a bare `L`) is usually clearer.

When **receiving**, `savebin` chooses the addresses: they come from the `S <start> <end>` the user types on the 1802, and nothing mem-xfr sends can change them. `-a` and `-l` are therefore treated as assertions -- mem-xfr reports an error if the data that actually arrives doesn't start where `-a` said or isn't as long as `-l` said. `-o` is where in the output file the first received byte is written; with `-x`, it is added to the addresses written into the hex records instead. Because `savebin` sends absolute addresses, a plain `mem-xfr -rx` records exactly the addresses the 1802 reported.

A nonzero `-o` on receive patches an existing file in place rather than truncating it, since truncating would discard the very bytes the offset was meant to skip past.

Numbers accept a `0x` prefix or an `h` suffix for hex; bare digits are decimal. (The monitor itself reads bare numbers as hex, but quietly turning `-l 1000` into 4096 bytes is worse than asking for one character of punctuation.)

#### Delay
`-d` works exactly as it does in max-xfr, and for the same reason: a bit-banged UART on the 1802 side has no hold register, so the CPU must already be polling for a start bit at the instant it arrives or the byte is lost outright. Start around 500-1000 microseconds for a bit-banged link; a hardware UART often needs little or none. Note that the delay goes *after* each byte mem-xfr originates, but *before* each echo or ack it sends in reply -- those are two opposite races, and both are real. (There is no `-b` or `-u` here; those are options on the ELF-DOS side, selecting which UART that end uses.)

### Options
```
-s  Send a memory image to the monitor's loadbin routine.
-r  Receive a memory image from the monitor's savebin routine.
-x  The local file is Intel hex rather than a raw binary.
-v  Verbose: show transfer statistics and per-block progress on stderr.
-d  Delay in microseconds after each byte sent, and before each echo or
    ack byte sent in reply.
-a  Sending: the 1802 address the image loads at (binary), or the address
    to start sending from (-x). Receiving: the address expected from the
    far end, verified against what actually arrives.
-o  Sending: the offset within the file to start at (binary), or an offset
    added to every record address (-x). Receiving: where in the output
    file the first byte goes (binary), or an offset added to the emitted
    addresses (-x).
-l  Sending: how many bytes to send. Receiving: the length expected,
    verified against what actually arrives.
```
### Protocol
Mem-xfr speaks the original, address-bearing MAX wire format, which `loadbin` and `savebin` already implement. The handshake is reversed between the two directions, and the host sends the final `'x'` in both -- each routine ends by requiring that byte, and returns with DF set if it doesn't arrive.

```
Sending, to loadbin:        Receiving, from savebin:
  ->  $55                     ->  $AA
  <-  $AA                     <-  $55
then, per block:            then, per block:
  ->  $01    <- echo          <-  $01    -> echo
  ->  cnt hi <- echo          <-  cnt hi -> echo
  ->  cnt lo <- echo          <-  cnt lo -> echo
  ->  adr hi <- echo          <-  adr hi -> echo
  ->  adr lo <- echo          <-  adr lo -> echo
  ->  cnt data bytes          <-  cnt data bytes
  <-  $AA                     ->  $AA
and to finish:              and to finish:
  ->  $00 (not echoed)        <-  $00 (not echoed)
  ->  'x'                     ->  'x'
```

Each byte of the 5-byte block header is echoed back and verified; the data bytes are not echoed, and are covered only by the single `$AA` that follows the block. Blocks are capped at 512 bytes, matching `savebin`'s own block size. A count of zero never appears in a header: `$00` in the command position is the end marker, and the 1802 side counts with the `UNTL` macro, which would treat a count of zero as 65536.
### Tests
`make test` runs a protocol test suite against Python mocks of `loadbin` and `savebin` transcribed from `max_mon.asm`. It needs no 1802 and no serial hardware -- it puts mem-xfr on one end of a pty and drives the other end itself -- so it runs anywhere the tool builds.

Testing against those mocks matters more than running mem-xfr against its own opposite mode would: a self round trip passes just as happily with the handshake inverted on both sides, so only a peer that independently implements what the 1802 actually does can confirm the wire format is right.

A few of the checks assert on *timing* rather than on bytes, which is deliberate. A pty has none of a bit-banged UART's timing, so a reply byte sent without its `-d` lead-in still arrives perfectly intact over a pty while being lost outright on real hardware -- which is exactly how the terminating `'x'` of a receive once shipped unpaced, hanging `savebin`. The idle gap before that byte is therefore measured and asserted directly. Anything new that mem-xfr sends *in reply* to the far end deserves the same treatment.
### Usage with minicom
As with max-xfr, these can be added as minicom file transfer protocols, for example:

       K  Ascii    /usr/local/bin/mem-xfr -sv -d 1000   Y   U   N   Y
       L  Ascii    /usr/local/bin/mem-xfr -rv -d 1000   Y   D   N   Y
## 1802 Code
The mr folder contains the source code for the loadbin subroutine, along with a simple demonstration of its use. The routine is callable via the standard SCRT mechanism. It takes a single parameter in the ra register. This parameter is the offset for the load address. Binary files are always transferred starting at address 0000h, so for binary files ra will specify the start address in memory where the file is to be loaded. In the case of an Intel hex file, the value in ra will be added as an offset to the address specified in the hex file.

The ms folder contains the source code for the savebin subroutine, along with a simple demonstration of its use. The routine is callable via the standard SCRT mechanism. It takes two parameters. Register ra contains the starting address of the memory buffer to be saved, and rc contains the number of bytes to be saved. If the file is saved in Intel hex format, the absolute address of the buffer will appear in the hex file.

The loadbin and savebin functions are included as part of the MAX Monitor program (https://github.com/arhefner/Elf-maxmon/tree/main), which contains standard vector entry points to allow them to be called from user software.
## Elf/OS
The ELfos folder contains a pair of file transfer utilities for Elf/OS.

The **mr** program is used to receive a file from another machine. It takes a single argument, which is the name of the file to be received. This is the Elf/OS name for the file; it does not need to be the same as the name of the file on the source machine.

The **ms** program is used to send a file from Elf/OS to another machine. It takes a single argument, which is the name of the file to be sent.
