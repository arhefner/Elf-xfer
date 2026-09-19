#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <termios.h>

#include "intl.h"
#include "ihex.h"

/*
 * mem-xfr - transfer a raw memory image to or from the MAX Monitor's own
 * loadbin/savebin routines (max_mon.asm in the Elf-maxmon project, also
 * reachable through its m_loadbin/m_savebin vectors, and the monitor's own
 * "L" and "S" commands).
 *
 * This is the "separate, dedicated utility" max-xfr's own header comment
 * promised when its Intel-HEX support and address-bearing wire format were
 * removed from it on 2026-09-04 (commits 94ebb48 and 6ad5007). The split is
 * not cosmetic -- the two tools speak genuinely different protocols for
 * genuinely different jobs:
 *
 *   max-xfr moves NAMED FILES. Its unit is "one file = one name + one
 *   contiguous byte stream", it has no concept of an address, and it can
 *   stream a file of any size because nothing in the protocol needs to know
 *   the total in advance.
 *
 *   mem-xfr moves a MEMORY IMAGE. Its unit is "these bytes belong at this
 *   1802 address". There are no filenames on the wire at all, and the whole
 *   address space is 64K, which is why this file is free to hold the image
 *   in one flat 64K array (see image[]/present[] below) where max-xfr
 *   deliberately streams instead.
 *
 * WIRE PROTOCOL (the original max-xfr format, preserved here verbatim
 * because loadbin/savebin are already deployed and speak exactly it -- this
 * tool is the one that had to be written to match them, not the reverse):
 *
 *   Sending, to loadbin:        Receiving, from savebin:
 *     ->  $55                     ->  $AA
 *     <-  $AA                     <-  $55
 *   then, per block:            then, per block:
 *     ->  $01   <- echo           <-  $01   -> echo
 *     ->  cnt hi <- echo          <-  cnt hi -> echo
 *     ->  cnt lo <- echo          <-  cnt lo -> echo
 *     ->  adr hi <- echo          <-  adr hi -> echo
 *     ->  adr lo <- echo          <-  adr lo -> echo
 *     ->  cnt data bytes          <-  cnt data bytes
 *     <-  $AA                     ->  $AA
 *   and to finish:              and to finish:
 *     ->  $00  (not echoed)       <-  $00  (not echoed)
 *     ->  'x'                     ->  'x'
 *
 * Note the handshake is reversed between the two directions, and that it is
 * the HOST that sends the final 'x' in both: loadbin ends with "read a byte,
 * it must be 'x'" (lbover) and savebin likewise (after its own $00), so that
 * byte is a required part of the protocol and not a stray -- without it
 * either routine returns with DF set and the monitor prints "error".
 *
 * The 5-byte block header is echo-verified byte by byte; the data bytes are
 * not echoed at all, and are covered only by the single $AA that follows the
 * block. A count of 0 never appears in a block header: $00 in the command
 * position is the end marker, and in any case the 1802 side counts with the
 * UNTL macro (DEC/GHI/XRI FF/BNZ), which treats a count of 0 as 65536.
 * Blocks are capped at 512 bytes to match savebin's own hardcoded block
 * size.
 *
 * BYTE PACING (-d) -- inherited wholesale from max-xfr, including the part
 * that took real hardware to find. A bit-banged UART on the 1802 side has no
 * hold register: the CPU must already be inside its own receive polling loop
 * when a start bit arrives, or the byte is simply gone. That gives the delay
 * two different placements, and they are not interchangeable:
 *
 *   send_byte()  sleeps AFTER writing. Used for bytes we originate, where
 *                the point is to let the far end finish its own bookkeeping
 *                and get back to f_read before the next byte arrives.
 *
 *   reply_byte() sleeps BEFORE writing. Used for the echoes and acks we send
 *                in response to the far end, where the race runs the other
 *                way: f_bread latches the first falling edge it sees as a
 *                start bit, with no idle-time requirement, so if our reply's
 *                start bit has already begun before savebin returns from
 *                f_type and calls f_read, it locks onto a later bit
 *                transition instead and samples the rest of the byte at the
 *                wrong offset. max-xfr commit 7da8310 fixed exactly this,
 *                and notes that delaying the far end's listen instead of our
 *                reply was tried first and made things uniformly worse.
 */

#define IMAGE_SIZE      65536       /* the 1802's whole address space */
#define MAX_BLOCK       512         /* matches savebin's own block size */

#define CMD_BLOCK       0x01
#define CMD_END         0x00
#define ACK             0xaa
#define SYNC            0x55
#define OVER            'x'

/*
 *	Globals.
 */
static int verbose = 0;
static int raw_mode = 0;
static struct timespec start;
static unsigned long bdone = 0;

static struct termios orig_termios;  /* TERMinal I/O Structure */
static int ttyfd = STDIN_FILENO;     /* STDIN_FILENO is 0 by default */

static int delay = 130;              /* Default value for 57.6k hw UART */

/* The image, plus a byte-per-address map of which locations a file or the
 * far end actually gave us. present[] is what lets a sparse Intel hex file
 * survive a round trip: without it there would be no way to tell a location
 * that was never mentioned from one that legitimately holds $00, and every
 * gap in the image would silently become a run of zeros on the wire. */
static uint8_t image[IMAGE_SIZE];
static uint8_t present[IMAGE_SIZE];

/*
 *	Show the transfer statistics.
 */
static void stats(void)
{
  struct timespec now;
  double elapsed;

  clock_gettime(CLOCK_MONOTONIC, &now);

  elapsed = (now.tv_sec - start.tv_sec) +
    (now.tv_nsec - start.tv_nsec) / 1000000000.0;

  if (elapsed <= 0.0) elapsed = 1e-9;

  fprintf(stderr, _("\r%.1f Kbytes transferred at %5d CPS"),
    (float)bdone / 1024, (int)(bdone / elapsed));
  fflush(stderr);
}

/*
 *	write_all: write() with retry on a short write or EINTR. Every raw
 *	byte this protocol sends goes through here.
 */
static int write_all(const void *buf, size_t len)
{
  const uint8_t *p = buf;
  while (len) {
    ssize_t ret = write(STDOUT_FILENO, p, len);
    if (ret < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, _("Error while writing (errno = %d)\n"), errno);
      return -1;
    }
    p += ret;
    len -= (size_t)ret;
  }
  return 0;
}

/*
 *	send_byte: write one byte we originate, then pace. reply_byte: pace,
 *	then write one byte we owe the far end in response to its own. See
 *	this file's header comment on BYTE PACING for why these two are not
 *	the same function.
 */
static int send_byte(uint8_t b)
{
  if (write_all(&b, 1) < 0) return -1;
  if (delay) usleep(delay);
  return 0;
}

static int reply_byte(uint8_t b)
{
  if (delay) usleep(delay);
  return write_all(&b, 1);
}

/*
 *	read_one_byte: read exactly one byte into *out, distinguishing a
 *	genuine timeout (tty_raw()'s own read timeout elapsed, read() returned
 *	0 and never touched the buffer) from a real read() failure. Reporting
 *	those two the same way is a mistake max-xfr made and had to fix twice
 *	(commits c17a633 and 7218c99) while chasing bit-bang hardware faults:
 *	on a timeout errno is untouched, so the old code printed a meaningless
 *	"errno = 0", and an uninitialized buffer printed a meaningless "got
 *	00" indistinguishable from the far end really having sent a zero.
 */
static int read_one_byte(const char *what, uint8_t *out)
{
  ssize_t ret = read(STDIN_FILENO, out, 1);

  if (ret == 0) {
    fprintf(stderr, "Error reading %s (timeout, no response)\n", what);
    return -1;
  }
  if (ret < 0) {
    fprintf(stderr, "Error reading %s (read error, errno = %d)\n", what, errno);
    return -1;
  }
  return 0;
}

/*
 *	read_expected_byte: read one byte and require it to equal `expected`.
 */
static int read_expected_byte(const char *what, uint8_t expected)
{
  uint8_t got;

  if (read_one_byte(what, &got) < 0) return -1;

  if (got != expected) {
    fprintf(stderr, "Error waiting for %s (got %02x, expected %02x)\n",
      what, got, expected);
    return -1;
  }
  return 0;
}

/*
 *	read_echo: read the far end's echo of a header byte we just sent and
 *	require it to match. loadbin echoes each of the five header bytes
 *	before it acts on them -- including the low address byte, which it
 *	echoes from the value as RECEIVED (plo r8) rather than after adding
 *	its own ra offset into r9, so a plain equality check stays correct
 *	even when the monitor's "L <offset>" is in use.
 */
static int read_echo(const char *what, uint8_t sent)
{
  uint8_t got;

  if (read_one_byte(what, &got) < 0) return -1;

  if (got != sent) {
    fprintf(stderr, "Error in echo of %s (sent %02x, got %02x)\n",
      what, sent, got);
    return -1;
  }
  return 0;
}

/*
 *	parse_num: accept a C-style "0x1234", a 1802-monitor-style "1234h",
 *	or a plain decimal number. A bare string of digits is DECIMAL -- the
 *	monitor itself would read it as hex, but silently reinterpreting
 *	"-l 1000" as 4096 bytes is worse than requiring one prefix or suffix,
 *	so the ambiguous case is resolved toward what a shell user expects and
 *	documented in the usage text.
 */
static int parse_num(const char *s, unsigned long *out, const char *what)
{
  char buf[64];
  char *end;
  size_t len;
  unsigned long v;

  len = strlen(s);
  if (len == 0 || len >= sizeof buf) {
    fprintf(stderr, "Invalid %s: \"%s\"\n", what, s);
    return -1;
  }
  memcpy(buf, s, len + 1);

  errno = 0;
  if (len > 1 && (buf[len - 1] == 'h' || buf[len - 1] == 'H')) {
    buf[len - 1] = 0;
    v = strtoul(buf, &end, 16);
  } else {
    v = strtoul(buf, &end, 0);
  }

  if (*end != 0 || end == buf || errno == ERANGE) {
    fprintf(stderr, "Invalid %s: \"%s\"\n", what, s);
    return -1;
  }

  *out = v;
  return 0;
}

/*
 *	load_bin: read a plain binary file into the image. -o selects where
 *	in the FILE to start, -l how many bytes to take, and -a the 1802
 *	address the first of those bytes belongs at. The window this returns
 *	is exactly what was loaded.
 */
static int load_bin(const char *path, unsigned long addr, unsigned long off,
  int l_given, unsigned long len, uint32_t *win_lo, uint32_t *win_hi)
{
  FILE *fp;
  long fsize;
  unsigned long want;

  if ((fp = fopen(path, "rb")) == NULL) {
    perror(path);
    return -1;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    perror(path);
    fclose(fp);
    return -1;
  }
  fsize = ftell(fp);
  if (fsize < 0) {
    perror(path);
    fclose(fp);
    return -1;
  }

  if (off > (unsigned long)fsize) {
    fprintf(stderr, "%s: -o offset %lu is past the end of the file "
      "(%ld bytes)\n", path, off, fsize);
    fclose(fp);
    return -1;
  }

  want = l_given ? len : (unsigned long)fsize - off;

  if (want == 0) {
    fprintf(stderr, "%s: nothing to send (0 bytes selected)\n", path);
    fclose(fp);
    return -1;
  }
  if (off + want > (unsigned long)fsize) {
    fprintf(stderr, "%s: -l length %lu from -o offset %lu runs past the end "
      "of the file (%ld bytes)\n", path, want, off, fsize);
    fclose(fp);
    return -1;
  }
  if (addr + want > IMAGE_SIZE) {
    fprintf(stderr, "-a address %04lxh plus %lu bytes runs past the top of "
      "the 1802's 64K address space\n", addr, want);
    fclose(fp);
    return -1;
  }

  if (fseek(fp, (long)off, SEEK_SET) != 0) {
    perror(path);
    fclose(fp);
    return -1;
  }
  if (fread(image + addr, 1, want, fp) != want) {
    fprintf(stderr, "%s: short read\n", path);
    fclose(fp);
    return -1;
  }
  fclose(fp);

  memset(present + addr, 1, want);
  *win_lo = (uint32_t)addr;
  *win_hi = (uint32_t)(addr + want);
  return 0;
}

/*
 *	load_hex: read an Intel hex file into the image, adding `reloc` (-o)
 *	to every record's own address. Each record's bytes are marked present,
 *	so gaps between records stay gaps and are never sent as zeros.
 */
static int load_hex(const char *path, unsigned long reloc)
{
  FILE *fp;
  char line[1024];
  int bytes[256];
  int hex_addr, n, status;
  int lineno = 0, saw_eof = 0, i;

  if ((fp = fopen(path, "r")) == NULL) {
    perror(path);
    return -1;
  }

  while (fgets(line, sizeof line, fp) != NULL) {
    const char *p = line;

    lineno++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == 0) continue;                  /* blank line */

    if (!parse_hex_line(p, bytes, &hex_addr, &n, &status)) {
      fprintf(stderr, "%s:%d: malformed Intel hex record\n", path, lineno);
      fclose(fp);
      return -1;
    }

    if (status == 0) {                      /* data */
      unsigned long base = (unsigned long)hex_addr + reloc;

      if (base + (unsigned long)n > IMAGE_SIZE) {
        fprintf(stderr, "%s:%d: record at %04xh (+ %lu offset) runs past the "
          "top of the 1802's 64K address space\n", path, lineno, hex_addr,
          reloc);
        fclose(fp);
        return -1;
      }
      for (i = 0; i < n; i++) {
        image[base + i] = (uint8_t)(bytes[i] & 0xff);
        present[base + i] = 1;
      }
    } else if (status == 1) {               /* end of file */
      saw_eof = 1;
      break;
    } else {
      /* Segment/linear extended-address records describe an address space
       * bigger than the 1802 has. Rejecting them beats loading their data
       * at a silently wrong address. */
      fprintf(stderr, "%s:%d: unsupported Intel hex record type %02x\n",
        path, lineno, status);
      fclose(fp);
      return -1;
    }
  }

  fclose(fp);

  if (!saw_eof) {
    fprintf(stderr, "%s: warning: no end-of-file record\n", path);
  }
  return 0;
}

/*
 *	image_extent: lowest present address, and one past the highest.
 *	Returns -1 if the image is empty.
 */
static int image_extent(uint32_t *lo, uint32_t *hi)
{
  uint32_t a;
  int found = 0;

  *lo = 0;
  *hi = 0;

  for (a = 0; a < IMAGE_SIZE; a++) {
    if (!present[a]) continue;
    if (!found) { *lo = a; found = 1; }
    *hi = a + 1;
  }
  return found ? 0 : -1;
}

/*
 *	send_block: one block -- the echo-verified 5-byte header, then the
 *	data, then the single $AA that covers it.
 */
static int send_block(const uint8_t *data, size_t count, uint16_t address)
{
  static const char *names[5] = {
    "command byte", "count high byte", "count low byte",
    "address high byte", "address low byte"
  };
  uint8_t hdr[5];
  size_t i;

  hdr[0] = CMD_BLOCK;
  hdr[1] = (uint8_t)((count >> 8) & 0xff);
  hdr[2] = (uint8_t)(count & 0xff);
  hdr[3] = (uint8_t)((address >> 8) & 0xff);
  hdr[4] = (uint8_t)(address & 0xff);

  for (i = 0; i < sizeof hdr; i++) {
    if (send_byte(hdr[i]) < 0) return -1;
    if (read_echo(names[i], hdr[i]) < 0) return -1;
  }

  for (i = 0; i < count; i++) {
    if (send_byte(data[i]) < 0) return -1;
  }

  return read_expected_byte("block ack", ACK);
}

/*
 *	send_image: hand the window [win_lo, win_hi) to loadbin, as one block
 *	per MAX_BLOCK bytes of each maximal run of present locations.
 */
static int send_image(uint32_t win_lo, uint32_t win_hi)
{
  uint32_t addr = win_lo;

  clock_gettime(CLOCK_MONOTONIC, &start);

  if (send_byte(SYNC) < 0) return -1;
  if (read_expected_byte("handshake ack", ACK) < 0) return -1;

  while (addr < win_hi) {
    uint32_t run_start, run_end, p;

    if (!present[addr]) { addr++; continue; }

    run_start = addr;
    while (addr < win_hi && present[addr]) addr++;
    run_end = addr;

    if (verbose) {
      fprintf(stderr, "\nSending %lu bytes to %04xh...\n",
        (unsigned long)(run_end - run_start), run_start);
    }

    for (p = run_start; p < run_end; ) {
      size_t count = run_end - p;

      if (count > MAX_BLOCK) count = MAX_BLOCK;
      if (send_block(image + p, count, (uint16_t)p) < 0) return -1;

      bdone += count;
      if (verbose) stats();
      p += (uint32_t)count;
    }
  }

  /* End marker, then the 'x' loadbin requires before it will return with
   * DF clear. Neither is echoed. */
  if (send_byte(CMD_END) < 0) return -1;
  if (send_byte(OVER) < 0) return -1;

  return 0;
}

/*
 *	recv_image: take a memory image from savebin into image[]/present[],
 *	reporting the address range that actually arrived.
 */
static int recv_image(uint32_t *got_lo, uint32_t *got_hi)
{
  int first = 1;

  *got_lo = 0;
  *got_hi = 0;

  clock_gettime(CLOCK_MONOTONIC, &start);

  if (send_byte(ACK) < 0) return -1;
  if (read_expected_byte("handshake sync", SYNC) < 0) return -1;

  for (;;) {
    uint8_t cmd, b;
    uint32_t count, address;
    size_t remaining;
    uint8_t *p;

    if (read_one_byte("command byte", &cmd) < 0) return -1;

    if (cmd == CMD_END) break;

    if (cmd != CMD_BLOCK) {
      fprintf(stderr, "Invalid command byte (got %02x, expected %02x "
        "or %02x)\n", cmd, CMD_BLOCK, CMD_END);
      return -1;
    }
    if (reply_byte(cmd) < 0) return -1;

    if (read_one_byte("count high byte", &b) < 0) return -1;
    count = (uint32_t)b << 8;
    if (reply_byte(b) < 0) return -1;

    if (read_one_byte("count low byte", &b) < 0) return -1;
    count |= b;
    if (reply_byte(b) < 0) return -1;

    if (read_one_byte("address high byte", &b) < 0) return -1;
    address = (uint32_t)b << 8;
    if (reply_byte(b) < 0) return -1;

    if (read_one_byte("address low byte", &b) < 0) return -1;
    address |= b;
    if (reply_byte(b) < 0) return -1;

    /* savebin never sends either of these, but a desynchronized link can
     * produce them, and reading 65536 bytes on a bad count would hang the
     * transfer rather than report it. */
    if (count == 0) {
      fprintf(stderr, "Invalid block count of 0 at address %04xh\n", address);
      return -1;
    }
    if (address + count > IMAGE_SIZE) {
      fprintf(stderr, "Block of %lu bytes at %04xh runs past the top of the "
        "1802's 64K address space\n", (unsigned long)count, address);
      return -1;
    }

    p = image + address;
    remaining = count;
    while (remaining) {
      ssize_t ret = read(STDIN_FILENO, p, remaining);
      if (ret == 0) {
        fprintf(stderr, "Error reading block data (timeout, no response)\n");
        return -1;
      }
      if (ret < 0) {
        fprintf(stderr, _("Read error (errno = %d)\n"), errno);
        return -1;
      }
      p += ret;
      remaining -= (size_t)ret;
    }
    memset(present + address, 1, count);

    if (first || address < *got_lo) *got_lo = address;
    if (first || address + count > *got_hi) *got_hi = address + count;
    first = 0;

    bdone += count;
    if (verbose) stats();

    if (reply_byte(ACK) < 0) return -1;
  }

  if (first) {
    fprintf(stderr, "No data received.\n");
    return -1;
  }

  /* savebin's own last act is to read this and require it to be 'x'. */
  if (send_byte(OVER) < 0) return -1;

  return 0;
}

/*
 *	store_bin: write the received image to a plain binary file, placing
 *	the byte from `base` at file offset `off`. Gaps in present[] are
 *	seeked over rather than zero-filled, so a sparse image stays sparse.
 *
 *	A nonzero -o against a file that already exists patches it in place
 *	("r+b") instead of truncating it -- that is the only reading of "start
 *	writing at offset N" that does not throw away the first N bytes of
 *	whatever was already there.
 */
static int store_bin(const char *path, uint32_t base, unsigned long off)
{
  FILE *fp;
  const char *mode = "wb";
  uint32_t addr = 0;

  if (off != 0) {
    FILE *probe = fopen(path, "rb");
    if (probe) {
      fclose(probe);
      mode = "r+b";
    }
  }

  if ((fp = fopen(path, mode)) == NULL) {
    perror(path);
    return -1;
  }

  while (addr < IMAGE_SIZE) {
    uint32_t run_start, run_end;
    size_t len;
    long pos;

    if (!present[addr]) { addr++; continue; }

    run_start = addr;
    while (addr < IMAGE_SIZE && present[addr]) addr++;
    run_end = addr;
    len = run_end - run_start;

    pos = (long)run_start - (long)base + (long)off;
    if (pos < 0) {
      fprintf(stderr, "%s: data at %04xh would land before the start of the "
        "file\n", path, run_start);
      fclose(fp);
      return -1;
    }
    if (fseek(fp, pos, SEEK_SET) != 0) {
      perror(path);
      fclose(fp);
      return -1;
    }
    if (fwrite(image + run_start, 1, len, fp) != len) {
      fprintf(stderr, "%s: write error\n", path);
      fclose(fp);
      return -1;
    }
  }

  if (fclose(fp) != 0) {
    perror(path);
    return -1;
  }
  return 0;
}

/*
 *	store_hex: write the received image as Intel hex, with `reloc` (-o)
 *	added to the addresses that go into the records. The addresses
 *	savebin sent are absolute, so with no -o they appear in the file
 *	exactly as the 1802 reported them.
 *
 *	hexout() closes fhex itself on its end=1 call -- hence no fclose here.
 */
static int store_hex(const char *path, unsigned long reloc)
{
  FILE *fp;
  uint32_t addr;

  if ((fp = fopen(path, "w")) == NULL) {
    perror(path);
    return -1;
  }

  for (addr = 0; addr < IMAGE_SIZE; addr++) {
    if (!present[addr]) continue;
    hexout(fp, image[addr], (int)(addr + reloc), 0);
  }

  hexout(fp, 0, 0, 1);
  return 0;
}

static void usage(void)
{
  fprintf(stderr, "\
Usage: mem-xfr -s [-v] [-x] [-d <delay>] [-a <addr>] [-o <off>] [-l <len>] <file>\n\
       mem-xfr -r [-v] [-x] [-d <delay>] [-a <addr>] [-o <off>] [-l <len>] <file>\n\
       -s:  send a memory image to the monitor's loadbin routine\n\
       -r:  receive a memory image from the monitor's savebin routine\n\
       -x:  <file> is an Intel hex file rather than a raw binary\n\
       -v:  verbose (statistics and per-block progress on stderr)\n\
       -d:  delay in microseconds after each byte sent, and before each\n\
            echo/ack byte sent in reply (default %d; a bit-banged UART on\n\
            the 1802 side typically needs 500-1000)\n\
       -a:  when sending, the 1802 address the image loads at (binary), or\n\
            the address to start sending from (-x); when receiving, the\n\
            address the far end is expected to send, verified against what\n\
            it actually sends\n\
       -o:  when sending, the offset within the file to start at (binary),\n\
            or an offset added to every record address (-x); when\n\
            receiving, where in the output file the first byte goes\n\
            (binary), or an offset added to the emitted addresses (-x)\n\
       -l:  when sending, how many bytes to send; when receiving, the\n\
            length expected, verified against what actually arrives\n\
       Numbers take a 0x prefix or an h suffix for hex; bare digits are\n\
       decimal.\n", delay);
  exit(1);
}

void fatal(char *message)
{
  fprintf(stderr,"fatal error: %s\n",message);
  exit(1);
}

/* reset tty - useful also for restoring the terminal when this process
   wishes to temporarily relinquish the tty
*/
int tty_reset(void)
{
    /* flush and reset */
    if (tcsetattr(ttyfd,TCSAFLUSH,&orig_termios) == 0)
    {
      raw_mode = 0;
      return 0;
    }

    return -1;
}

/* exit handler for tty reset */

/* NOTE: If the program terminates due to a signal   */
/* this code will not run.  This is for exit()'s     */
/* only.  For resetting the terminal after a signal, */
/* a signal handler which calls tty_reset is needed. */
void tty_atexit(void)
{
  if (raw_mode)
  {
    tty_reset();
  }
}

/* put terminal in raw mode - see termio(7I) for modes */
void tty_raw(void)
{
  struct termios raw;

  raw = orig_termios;  /* copy original and then modify below */

  /* input modes - clear indicated ones giving: no break, no CR to NL,
     no parity check, no strip char, no start/stop output (sic) control */
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  /* output modes - clear giving: no post processing such as NL to CR+NL */
  raw.c_oflag &= ~(OPOST);

  /* control modes - set 8 bit chars */
  raw.c_cflag |= (CS8);

  /* local modes - clear giving: echoing off, canonical off (no erase with
     backspace, ^U,...),  no extended functions, no signal chars (^Z,^C) */
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  /* Return from read() on the first byte, or after 25.5 seconds (VTIME is
   * one byte, in tenths of a second, so this is its maximum) with none.
   * max-xfr settled on this same value for a reason that applies here
   * verbatim: mem-xfr -r writes its own $AA and then blocks waiting for the
   * far end's $55, and the user has to physically get to the other machine
   * and type the monitor's "S <start> <end>" in between. Since this setting
   * governs every read() for the life of the process, a long timeout costs
   * nothing in correctness -- a link where nothing ever answers still times
   * out and still reports accurately, it just takes longer to say so. */
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 255;

  /* put terminal in raw mode after flushing */
  if (tcsetattr(ttyfd,TCSAFLUSH,&raw) < 0) fatal("can't set raw mode");

  raw_mode++;
}

int main(int argc, char **argv)
{
  int c;
  int what = 0, hexmode = 0;
  int a_given = 0, l_given = 0;
  unsigned long addr = 0, off = 0, len = 0;
  const char *file;
  uint32_t win_lo = 0, win_hi = 0;
  int ret;

  while ((c = getopt(argc, argv, "a:d:l:o:rsvx")) != EOF) {
    switch (c) {
      case 'a':
        if (parse_num(optarg, &addr, "-a address") < 0) exit(1);
        a_given = 1;
        break;
      case 'd':
        delay = atoi(optarg);
        break;
      case 'l':
        if (parse_num(optarg, &len, "-l length") < 0) exit(1);
        l_given = 1;
        break;
      case 'o':
        if (parse_num(optarg, &off, "-o offset") < 0) exit(1);
        break;
      case 's':
      case 'r':
        what = c;
        break;
      case 'v':
        verbose++;
        break;
      case 'x':
        hexmode = 1;
        break;
      case '?':
        fprintf(stderr, "Unknown option: %c\n", optopt);
        usage();
        break;
      default:
        usage();
        break;
    }
  }

  if (what == 0) usage();
  if (optind != argc - 1) usage();          /* exactly one file, either way */
  file = argv[optind];

  if (delay < 0) {
    fprintf(stderr, "Invalid -d delay: %d\n", delay);
    exit(1);
  }
  if (a_given && addr >= IMAGE_SIZE) {
    fprintf(stderr, "-a address %04lxh is outside the 1802's 64K address "
      "space\n", addr);
    exit(1);
  }
  if (l_given && (len == 0 || len > IMAGE_SIZE)) {
    fprintf(stderr, "-l length %lu is not between 1 and %d\n", len,
      IMAGE_SIZE);
    exit(1);
  }

  /* Read the whole input before touching the tty: a bad file should fail
   * before the far end has been told anything at all. */
  if (what == 's') {
    if (hexmode) {
      uint32_t lo, hi;

      if (load_hex(file, off) < 0) exit(1);
      if (image_extent(&lo, &hi) < 0) {
        fprintf(stderr, "%s: no data records\n", file);
        exit(1);
      }
      win_lo = a_given ? (uint32_t)addr : lo;
      win_hi = l_given ? win_lo + (uint32_t)len : hi;
      if (win_hi > IMAGE_SIZE) win_hi = IMAGE_SIZE;
      if (win_lo >= win_hi) {
        fprintf(stderr, "%s: nothing to send in %04xh..%04xh (the file "
          "covers %04xh..%04xh)\n", file, win_lo, win_hi, lo, hi - 1);
        exit(1);
      }
    } else {
      if (load_bin(file, a_given ? addr : 0, off, l_given, len,
        &win_lo, &win_hi) < 0) exit(1);
    }
  }

  /* check that input is from a tty */
  if (! isatty(ttyfd)) fatal("not on a tty");

  /* store current tty settings in orig_termios */
  if (tcgetattr(ttyfd,&orig_termios) < 0) fatal("can't get tty settings");

  /* register the tty reset with the exit handler */
  if (atexit(tty_atexit) != 0) fatal("atexit: can't register tty reset");

  tty_raw();      /* put tty in raw mode */

  if (what == 's') {
    if (verbose) {
      fprintf(stderr, _("Sending %s\n"), file);
      fflush(stderr);
    }
    ret = send_image(win_lo, win_hi);
    if (ret == 0 && verbose) {
      /* Also supplies the newline that terminates stats()' own \r-prefixed
       * progress line, so "... Done." doesn't land on the end of it. */
      fprintf(stderr, "\n%lu bytes sent (%04xh..%04xh)\n", bdone, win_lo,
        win_hi - 1);
    }
  } else {
    uint32_t got_lo = 0, got_hi = 0;

    if (verbose) {
      fprintf(stderr, _("Receiving into %s\n"), file);
      fflush(stderr);
    }
    ret = recv_image(&got_lo, &got_hi);

    /* -a and -l are assertions on this side: savebin's own start address
     * and byte count come from the monitor's "S <start> <end>", so this
     * tool cannot steer them -- it can only confirm that what arrived is
     * what was meant to arrive. */
    if (ret == 0 && a_given && got_lo != addr) {
      fprintf(stderr, "\n-a said %04lxh but the far end sent data starting "
        "at %04xh\n", addr, got_lo);
      ret = -1;
    }
    if (ret == 0 && l_given && (got_hi - got_lo) != len) {
      fprintf(stderr, "\n-l said %lu bytes but the far end sent %lu "
        "(%04xh..%04xh)\n", len, (unsigned long)(got_hi - got_lo), got_lo,
        got_hi - 1);
      ret = -1;
    }

    if (ret == 0) {
      if (hexmode) {
        ret = store_hex(file, off);
      } else {
        ret = store_bin(file, got_lo, off);
      }
      if (ret == 0 && verbose) {
        fprintf(stderr, "\nReceived %lu bytes (%04xh..%04xh) into %s\n",
          (unsigned long)(got_hi - got_lo), got_lo, got_hi - 1, file);
      }
    }
  }

  tty_reset();

  if (verbose) {
    fprintf(stderr, _("... Done.\n"));
    fflush(stderr);
  }

  return ret < 0 ? 1 : 0;
}
