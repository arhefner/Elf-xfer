#include <assert.h>
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

/*
 * Batch-capable MAX protocol, length-prefixed/doubly-acknowledged
 * chunk design (2026-09-04, replacing the earlier 2026-09-01 command-
 * byte/address-field framing entirely). See progs/mr.asm's own header
 * comment (the ELF-DOS side of this protocol) for the full wire-level
 * writeup; this file mirrors that design exactly, just with the
 * sender/receiver roles swapped for -s/-r respectively.
 *
 * Every chunk (a file's header, or one piece of its data) is: a 2-
 * byte big-endian LENGTH, then wait for a $AA ack; if LENGTH is
 * nonzero, that many payload bytes (paced by "-d"), then wait for a
 * SECOND $AA ack. A LENGTH of 0 is an end marker -- of the current
 * file's data if a header is expected next, or of the whole batch if
 * a header chunk itself has LENGTH 0 -- and needs only the one ack
 * (no payload phase). No command byte, no address field -- both are
 * gone; a chunk's own length is the only framing this protocol needs.
 *
 * This closes two real hardware bugs found on two different days,
 * both the identical class: the sender getting ahead of the receiver
 * on hardware with no UART FIFO to absorb it. The first (2026-09-04,
 * fixed by this whole chunk redesign): the OLD protocol's un-echoed,
 * un-acknowledged end-of-file/end-of-batch marker bytes were being
 * silently dropped, since nothing throttled the sender from writing
 * them back-to-back right after the receiver's slowest step (mr.asm's
 * own real disk write for a file's last data block) -- every OTHER
 * byte in the old protocol was already naturally throttled (echo-
 * verified header fields, or the per-byte "-d" delay), so this
 * redesign extends that same "the sender can never get ahead"
 * property to the whole protocol, uniformly: each chunk's PAYLOAD ack
 * is sent only once the receiver has genuinely finished processing
 * that chunk (the disk write, or opening the destination file), not
 * merely once the bytes are off the wire. The second bug (same day, in
 * this redesign's own first draft): a zero-length chunk's ONE ack was
 * being sent unconditionally, immediately upon reading the length --
 * safe in isolation, but on the receiving side (mr.asm) that ack was
 * sent BEFORE closing the file the marker refers to, letting the
 * sender race ahead and write the NEXT thing while the receiver was
 * still mid-close. Fixed the identical way: the zero-length chunk's
 * ack is now deferred to whichever caller actually knows when its own
 * processing (closing a file, or nothing at all for the outer batch-
 * end marker) is genuinely done -- see recv_chunk()'s own comment.
 *
 * BIT-BANG UART FIX (2026-09-05): a real hardware UART has at least a
 * one-byte hold register that captures an incoming byte automatically,
 * regardless of what the receiving CPU happens to be doing at that
 * instant. A bit-banged UART has none of that -- the CPU itself has to
 * be inside its own polling loop for the entire duration a bit
 * arrives, or it's simply gone. The 2-byte length field used to go out
 * unpaced (a single write_all(hdr, 2)), which was fine on a real UART
 * but could make a bit-banged receiver miss the second byte's start
 * bit entirely if it hadn't yet finished the bookkeeping between its
 * own two per-byte reads -- see send_chunk()'s own comment for the
 * full account. Also found along the way: every ack/handshake check in
 * this file used to read `x` and print it unconditionally on any
 * failure, without checking whether read() actually returned a byte at
 * all -- a genuine timeout (tty_raw()'s own read timeout, see its own
 * comment for the current value and history) leaves the buffer
 * untouched, so a slow-to-respond far end produced a
 * misleading "ack = 00" indistinguishable from a real wrong byte.
 * read_expected_byte() now reports which one actually happened.
 *
 * The OLD single-block protocol (no header, no batch, one file per
 * invocation) is gone -- mr.asm/ms.asm no longer speak it, so this
 * tool can't either. This also means the old Intel-HEX (.hex/.intel)
 * support -- which encoded a memory image as one or more separately-
 * addressed chunks, a shape with no coherent mapping onto "one file =
 * one name + one contiguous byte stream" -- has been dropped along
 * with it; that mode was never meaningfully usable against mr.asm
 * anyway, since mr.asm never had any real use for a wire-level address
 * field to begin with. ihex.c/ihex.h themselves were removed
 * (2026-09-05) rather than left unlinked -- BIOS-level hex/binary
 * transfers are getting their own separate, dedicated utility instead,
 * since trying to make one tool do both this batch file-transfer
 * protocol and raw memory-image transfer at once had already proven
 * unwieldy.
 *
 * Streaming, not buffering: file data is read/written directly to/
 * from disk in BLOCK_BUF_LEN-byte chunks as it's sent/received,
 * instead of loading a whole file into one fixed-size array first --
 * avoids a 64KB-per-file ceiling (a real overflow risk once files
 * could legitimately exceed that).
 */

#define BLOCK_BUF_LEN       512
#define MAXFER_NAME_MAX     127
#define HOST_PATH_MAX       1024

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

/*
 *	Show the up/download statistics.
 */
static void stats()
{
  struct timespec now;
  double elapsed;

  clock_gettime(CLOCK_MONOTONIC, &now);

  elapsed = (now.tv_sec - start.tv_sec) +
    (now.tv_nsec - start.tv_nsec) / 1000000000.0;

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
 *	host_basename: extract the text after the last '/' in path (or the
 *	whole string if there is none) into buf, bounded to
 *	MAXFER_NAME_MAX chars -- matches mr.asm/ms.asm's own identical cap
 *	and scan logic exactly, rather than relying on a platform's own
 *	basename() (which can modify its argument, and whose trailing-
 *	slash/empty-result behavior isn't perfectly consistent across
 *	implementations).
 */
static void host_basename(const char *path, char *buf, size_t bufcap)
{
  const char *base = path;
  const char *p;
  size_t len;

  for (p = path; *p; p++) {
    if (*p == '/') base = p + 1;
  }

  len = strlen(base);
  if (len > MAXFER_NAME_MAX) len = MAXFER_NAME_MAX;
  if (len >= bufcap) len = bufcap - 1;
  memcpy(buf, base, len);
  buf[len] = 0;
}

/*
 *	read_expected_byte: read a single byte and check it against
 *	`expected`, with an error message that distinguishes a genuine
 *	timeout (no byte arrived within tty_raw()'s own read timeout, see
 *	its own comment for the current value) from a real byte mismatch,
 *	rather than printing whatever uninitialized stack garbage happened
 *	to be sitting in the read buffer on a timeout.
 *
 *	Found 2026-09-05 chasing an intermittent bit-bang-UART hardware
 *	failure: every "ack"/"handshake" check in this file used to do
 *	`read(fd, &x, 1) != 1 || x != expected` directly and print `x`
 *	unconditionally -- but read() returning 0 (a genuine timeout, not
 *	an error) never touches `x` at all, so a slow or not-yet-ready far
 *	end produced a MISLEADING "ack = 00" report indistinguishable from
 *	the far end genuinely having sent a wrong byte. Both are real
 *	possibilities on a bit-banged receiver (see send_chunk's own header
 *	comment), and telling them apart matters for diagnosing which one
 *	actually happened.
 *
 *	what: a short description used in the error message (e.g. "length
 *	ack", "handshake sync").
 *	Returns 0 (the expected byte was received), -1 (timeout, mismatch,
 *	or a genuine read error -- already reported to stderr).
 */
static int read_expected_byte(const char *what, uint8_t expected)
{
  uint8_t got;
  ssize_t ret = read(STDIN_FILENO, &got, 1);

  if (ret == 0) {
    fprintf(stderr, "Error waiting for %s (timeout, no response)\n", what);
    return -1;
  }
  if (ret < 0) {
    fprintf(stderr, "Error waiting for %s (read error, errno = %d)\n",
      what, errno);
    return -1;
  }
  if (got != expected) {
    fprintf(stderr, "Error waiting for %s (got %02x, expected %02x)\n",
      what, got, expected);
    return -1;
  }
  return 0;
}

/*
 *	read_one_byte: read exactly one byte into *out, with an error
 *	message that distinguishes a genuine timeout from a real read()
 *	failure -- see read_expected_byte()'s own comment for why that
 *	distinction matters. Used by recv_chunk() for the 2-byte length
 *	field, where (unlike read_expected_byte()) there's no single fixed
 *	value to compare against.
 *
 *	Found 2026-09-05: this exact site was still printing "Read error
 *	(errno = 0)" on a genuine timeout mid-transfer (errno is untouched
 *	by a read() that returns 0), on the largest file sent so far over
 *	a bit-banged connection -- left unfixed at the time
 *	read_expected_byte() was introduced, since it doesn't compare
 *	against one fixed value, but the same ambiguity applies here too.
 *
 *	Returns 0 (byte received), -1 (timeout or read error -- already
 *	reported).
 */
static int read_one_byte(uint8_t *out)
{
  ssize_t ret = read(STDIN_FILENO, out, 1);

  if (ret == 0) {
    fprintf(stderr, "Read error (timeout, no response)\n");
    return -1;
  }
  if (ret < 0) {
    fprintf(stderr, "Read error (errno = %d)\n", errno);
    return -1;
  }
  return 0;
}

/*
 *	send_chunk: write a chunk's 2-byte big-endian length (each byte
 *	paced by "-d", same as payload -- see below for why), wait for the
 *	length ack, then -- if len is nonzero -- write that many payload
 *	bytes (also paced) and wait for a SECOND ack. A len==0 call
 *	(payload may be NULL) sends only the length and its one ack --
 *	this is how a "no more data"/"no more files" end marker is sent,
 *	there's no separate function or wire byte for it. See this file's
 *	own header comment for why the payload ack specifically has to
 *	come from the receiver only once it has finished ITS OWN
 *	processing of the chunk, not just once the bytes are off the wire
 *	-- that's what makes this whole design self-throttling.
 *
 *	Pacing the length bytes too (2026-09-05, real hardware bug on a
 *	bit-banged receiver): a real hardware UART has at least a one-byte
 *	hold register that captures an incoming byte regardless of what
 *	the receiving CPU happens to be doing at that instant -- payload
 *	bytes already relied on that margin being enough on their own even
 *	before this fix, via "-d" pacing giving the CPU time to finish its
 *	own bookkeeping and get back to reading. A bit-banged UART has NO
 *	such buffer at all: the CPU itself has to be inside its own receive
 *	polling loop for the entire duration a bit arrives, or that bit is
 *	gone, not queued. The 2-byte length field used to go out back-to-
 *	back with zero pacing (write_all(hdr, 2)) -- harmless on a real
 *	UART, but on mr.asm's bit-bang receiver the handful of instructions
 *	between its own two per-byte reads (storing the first byte before
 *	reading the second) was sometimes enough to miss the second byte's
 *	start bit entirely, producing exactly the intermittent failures
 *	this was found from: sometimes failing at the handshake itself
 *	(also unpaced, see send_batch below), sometimes one step further
 *	at this length field -- never at the same point twice, consistent
 *	with marginal timing rather than a deterministic protocol bug.
 *
 *	Returns 0 on success, -1 on any ack mismatch/read failure (fatal --
 *	the two ends are now out of lock-step).
 */
static int send_chunk(const uint8_t *payload, size_t len)
{
  uint8_t hdr[2];
  size_t i;

  hdr[0] = (uint8_t)((len >> 8) & 0xff);
  hdr[1] = (uint8_t)(len & 0xff);

  if (write_all(&hdr[0], 1) < 0) return -1;
  if (delay) usleep(delay);
  if (write_all(&hdr[1], 1) < 0) return -1;
  if (delay) usleep(delay);

  if (read_expected_byte("length ack", 0xaa) < 0) return -1;

  if (len == 0) return 0;

  for (i = 0; i < len; i++) {
    if (write_all(payload + i, 1) < 0) return -1;
    if (delay) usleep(delay);
  }

  /* NOT counted/reported here via stats() -- send_batch() credits
   * real DATA chunks only, once it knows what it's counting; see
   * recv_chunk()'s own comment on the receiving side for why a
   * header's own bytes are kept out of the "Kbytes transferred"
   * running total. */

  if (read_expected_byte("payload ack", 0xaa) < 0) return -1;

  return 0;
}

/*
 *	recv_chunk: read one chunk's 2-byte big-endian length. If the
 *	length is 0, that's the whole exchange -- no payload follows, and
 *	NO ack is sent here: the caller must call send_chunk_ack() once it
 *	has finished whatever a zero-length chunk implies on its own end
 *	(closing the file it was just writing, for instance) -- deferred
 *	for the identical reason a nonzero chunk's PAYLOAD ack is deferred
 *	(see below): acking before that finishes would let the sender race
 *	ahead of processing we haven't actually completed yet. If nonzero,
 *	ack the length immediately (safe here, since nothing slow happens
 *	before the payload itself starts flowing), read that many payload
 *	bytes into buf, but do NOT ack the payload -- the caller calls
 *	send_chunk_ack() once it has genuinely finished its own processing
 *	of it (a disk write, or opening the destination file), which is
 *	the whole point of this design: the sender can never get more than
 *	one length-field ahead of what the receiver has actually finished
 *	with. (On the ELF-DOS side, the zero-length case originally acked
 *	immediately too, and a real hardware hang on 2026-09-04 traced
 *	directly to that: the sender raced ahead and wrote the next thing
 *	while mr.asm was still closing the file, on hardware with no UART
 *	FIFO to absorb it. Applied the same fix here for symmetry/
 *	correctness even though a fast host with real OS-level buffering
 *	is unlikely to ever hit it in practice.)
 *
 *	Returns 1 (length was 0 -- an end marker; an ack is still owed via
 *	send_chunk_ack()), 0 (a real chunk: buf[0..*out_len-1] holds its
 *	payload, the length has already been ack'd but the PAYLOAD ack is
 *	still owed via send_chunk_ack()), or -1 (fatal: a read failed, or
 *	the chunk was too large for buf).
 */
static int recv_chunk(uint8_t *buf, size_t bufcap, size_t *out_len)
{
  uint8_t hdr[2];
  uint16_t count;
  size_t remaining;
  uint8_t *p;

  if (read_one_byte(&hdr[0]) < 0) return -1;
  if (read_one_byte(&hdr[1]) < 0) return -1;
  count = ((uint16_t)hdr[0] << 8) | hdr[1];

  if (count == 0) return 1;

  {
    uint8_t ack = 0xaa;
    /* Paced the same as send_chunk_ack() below -- see its own comment
     * for why: on a bit-banged receiver (ms.asm), f_bread locks onto
     * whatever falling edge it sees first as the start bit, with no
     * idle-time requirement of its own -- if OUR ack's start bit
     * begins before the sender has even returned to its own listen
     * call, the real start bit is missed entirely and a LATER bit
     * transition gets mistaken for one instead, producing junk. */
    if (delay) usleep(delay);
    if (write_all(&ack, 1) < 0) return -1;
  }

  if (count > bufcap) {
    fprintf(stderr, "Block too large (%u bytes).\n", (unsigned)count);
    return -1;
  }

  p = buf;
  remaining = count;
  while (remaining) {
    ssize_t ret = read(STDIN_FILENO, p, remaining);
    if (ret == 0) {
      fprintf(stderr, "Read error (timeout, no response)\n");
      return -1;
    }
    if (ret < 0) {
      fprintf(stderr, _("Read error (errno = %d)\n"), errno);
      return -1;
    }
    p += ret;
    remaining -= (size_t)ret;
  }

  /* NOT counted/reported here via stats() -- this fires for a
   * header's own payload too, which receive_batch() hasn't parsed
   * into a filename yet at this point, so a stats() call here would
   * print a "Kbytes transferred" line before the matching "Receiving
   * <name>..." announcement -- confusing on the very first file (the
   * only place a reader could ever actually see it as its own,
   * distinct line, since bdone starts at exactly 0). receive_batch()
   * credits real DATA chunks only, once it knows what it's counting. */

  *out_len = count;
  return 0;
}

/*
 *	send_chunk_ack: write a chunk's payload ack. Called by
 *	receive_batch() only once it has finished its own processing of a
 *	chunk recv_chunk() reported as "real" (return value 0) -- see this
 *	file's own header comment and recv_chunk()'s own for why that
 *	timing is the whole point.
 *
 *	Paced with "-d" before the byte goes out (2026-09-06, real
 *	hardware bug on ms.asm's bit-banged receiver): this ack used to go
 *	out completely unpaced, the one place in this whole protocol that
 *	had no delay of any kind. Confirmed the actual mechanism directly
 *	with the ms.asm author: f_bread (the bit-bang UART read routine)
 *	recognizes a falling edge as a start bit immediately, with no
 *	requirement that the line have been idle for any minimum time
 *	first -- if our ack's own start bit has ALREADY begun by the time
 *	the sender gets back around to calling f_bread (returning from its
 *	own send loop, a few more instructions, then the call itself),
 *	f_bread has nothing to catch; it simply hasn't started polling
 *	yet. It then locks onto whichever LATER bit transition it sees
 *	first as if that were the start bit, sampling at the wrong offset
 *	for the rest of that byte -- explains a real hardware finding
 *	exactly: a large (512-byte, the fullest single chunk this protocol
 *	sends) payload's own ack reliably came back wrong even though the
 *	payload itself was received correctly, while every smaller
 *	chunk's ack (headers, sub-512-byte data) succeeded -- consistent
 *	with the sender's own return-from-send-loop overhead only
 *	occasionally landing on the wrong side of this race, not a random
 *	glitch. (The opposite fix -- delaying the SENDER's own listen
 *	instead of the receiver's ack -- was tried first and made things
 *	uniformly worse, breaking every chunk instead of just the large
 *	one: it only widens the same race rather than closing it.) This
 *	fixes it at the true source of the timing mismatch: give the
 *	sender time to get back to listening BEFORE we ever start sending
 *	the ack, rather than trying to guess how long the sender itself
 *	might need to wait.
 */
static int send_chunk_ack(void)
{
  uint8_t ack = 0xaa;
  if (delay) usleep(delay);
  return write_all(&ack, 1);
}

/*
 *	send_batch: send "files" (nfiles entries) to a batch-capable
 *	receiver (mr.asm on the ELF-DOS side, or another instance of this
 *	same tool run with -r). See this file's own header comment for the
 *	overall protocol shape.
 *
 *	Handshake timing: we send $55 immediately, unconditionally, before
 *	looking at any file at all -- the receiver blocks passively
 *	waiting for it with no way to know in advance whether any of our
 *	files are even real, matching the original single-file protocol's
 *	own identical handshake timing (and mr.asm's own header comment on
 *	why: unlike our own deferred-until-something-real-to-send
 *	handshake on the SENDING side in ms.asm, the RECEIVING side here
 *	has no such option).
 *
 *	A local failure for one file (doesn't exist, is a directory, can't
 *	open) prints its own message and moves on to the next file. A
 *	wire/protocol failure, or a local read error partway through a
 *	file whose header has already gone out, aborts the whole batch
 *	immediately -- the receiver is either out of lock-step with us, or
 *	already mid-file expecting data we can no longer produce, and this
 *	protocol has no mid-file abort/resync signal (matching mr.asm/
 *	ms.asm's own identical policy).
 *
 *	Returns 0 if every file was sent with no local or protocol errors,
 *	-1 otherwise.
 */
static int send_batch(char **files, int nfiles)
{
  int i;
  int any_ok = 0, any_err = 0, fatal = 0;
  uint8_t sync = 0x55;

  clock_gettime(CLOCK_MONOTONIC, &start);

  if (write_all(&sync, 1) < 0) return -1;
  if (read_expected_byte("handshake ack", 0xaa) < 0) return -1;

  for (i = 0; i < nfiles; i++) {
    const char *path = files[i];
    struct stat st;
    FILE *fp;
    char base[MAXFER_NAME_MAX + 1];
    uint8_t hdr[MAXFER_NAME_MAX + 5];
    size_t namelen, hdrlen;
    uint32_t size32;
    uint8_t chunk[BLOCK_BUF_LEN];
    size_t n;
    int file_ok;

    if (stat(path, &st) != 0) {
      fprintf(stderr, "%s: %s\n", path, strerror(errno));
      any_err++;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      fprintf(stderr, "%s: is a directory\n", path);
      any_err++;
      continue;
    }

    fp = fopen(path, "rb");
    if (!fp) {
      fprintf(stderr, "%s: %s\n", path, strerror(errno));
      any_err++;
      continue;
    }

    host_basename(path, base, sizeof(base));
    namelen = strlen(base);
    memcpy(hdr, base, namelen);
    hdr[namelen] = 0;
    /* NB: size32 truncates a real file size wider than 32 bits to fit
     * the wire's own 32-bit field, matching the width ELF-DOS's own
     * FCB_FSIZE holds -- not expected to matter in practice. */
    size32 = (uint32_t)st.st_size;
    hdr[namelen + 1] = (uint8_t)((size32 >> 24) & 0xff);
    hdr[namelen + 2] = (uint8_t)((size32 >> 16) & 0xff);
    hdr[namelen + 3] = (uint8_t)((size32 >> 8) & 0xff);
    hdr[namelen + 4] = (uint8_t)(size32 & 0xff);
    hdrlen = namelen + 5;

    if (verbose) {
      fprintf(stderr, "Sending %s (%lu bytes)...\n", base,
        (unsigned long)st.st_size);
    }

    if (send_chunk(hdr, hdrlen) < 0) {
      fclose(fp);
      fatal = 1;
      break;
    }

    file_ok = 1;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
      if (send_chunk(chunk, n) < 0) {
        file_ok = 0;
        break;
      }
      bdone += n;
      if (verbose) stats();
    }
    if (file_ok && ferror(fp)) {
      fprintf(stderr, "%s: read error\n", path);
      file_ok = 0;
    }
    fclose(fp);

    if (!file_ok) {
      fatal = 1;
      break;
    }

    if (send_chunk(NULL, 0) < 0) {     /* end of THIS file's data */
      fatal = 1;
      break;
    }
    any_ok++;
  }

  if (!fatal) {
    if (send_chunk(NULL, 0) < 0) fatal = 1;    /* outer "no more files" */
  }

  if (verbose) {
    fprintf(stderr, "\n%d file(s) sent", any_ok);
    if (any_err) fprintf(stderr, ", %d failed", any_err);
    fprintf(stderr, ".\n");
  }

  return (fatal || any_err) ? -1 : 0;
}

/*
 *	receive_batch: receive a batch of one or more files from a
 *	batch-capable sender (ms.asm on the ELF-DOS side, or another
 *	instance of this same tool run with -s). See this file's own
 *	header comment for the overall protocol shape.
 *
 *	dest_arg == NULL: batch mode, write each file under its own
 *	transmitted name into the current directory.
 *	dest_arg != NULL, dest_is_dir: batch mode into that directory.
 *	dest_arg != NULL, !dest_is_dir: single-file mode -- dest_arg is
 *	used verbatim as the destination for the FIRST file received,
 *	ignoring that file's own transmitted name; any further files in
 *	the same session are drained (read and discarded) so the session
 *	still ends cleanly -- matches mr.asm's own identical semantics
 *	exactly (this function is this tool's own mirror of mr_session).
 *
 *	Returns 0 if every file was received with no local or protocol
 *	errors, -1 otherwise.
 */
static int receive_batch(const char *dest_arg, int dest_is_dir)
{
  uint8_t ack = 0xaa;
  int expecting_header = 1;
  int any_ok = 0, any_err = 0, fatal = 0;
  int single_used = 0;
  int discarding = 0;
  FILE *out = NULL;
  char cur_name[MAXFER_NAME_MAX + 1];
  char destpath[HOST_PATH_MAX];
  uint32_t cur_size;
  uint8_t buf[BLOCK_BUF_LEN];

  clock_gettime(CLOCK_MONOTONIC, &start);

  if (write_all(&ack, 1) < 0) return -1;
  if (read_expected_byte("handshake sync", 0x55) < 0) return -1;

  for (;;) {
    size_t len = 0;
    int r = recv_chunk(buf, sizeof(buf), &len);

    if (r < 0) { fatal = 1; break; }

    if (r == 1) {
      /* end marker -- ack deferred from recv_chunk until our own
       * processing (if any) is done, see recv_chunk's own comment */
      if (expecting_header) {
        /* end of the whole batch -- nothing to close, ack right away */
        if (send_chunk_ack() < 0) fatal = 1;
        break;
      }

      /* end of the CURRENT file's data */
      if (out) { fclose(out); out = NULL; any_ok++; }
      discarding = 0;
      if (send_chunk_ack() < 0) { fatal = 1; break; }
      expecting_header = 1;
      continue;
    }

    /* r == 0: a real chunk, payload ack still owed */
    if (expecting_header) {
      size_t real_namelen = 0;
      while (real_namelen < len && buf[real_namelen] != 0) real_namelen++;

      if (real_namelen >= len) {
        strcpy(cur_name, "?");
        cur_size = 0;
      } else {
        size_t copy_len = real_namelen;
        size_t size_off = real_namelen + 1;

        if (copy_len > MAXFER_NAME_MAX) copy_len = MAXFER_NAME_MAX;
        memcpy(cur_name, buf, copy_len);
        cur_name[copy_len] = 0;

        if (len - size_off >= 4) {
          cur_size = ((uint32_t)buf[size_off] << 24) |
                     ((uint32_t)buf[size_off + 1] << 16) |
                     ((uint32_t)buf[size_off + 2] << 8) |
                     (uint32_t)buf[size_off + 3];
        } else {
          cur_size = 0;
        }
      }

      discarding = 0;

      if (dest_arg == NULL) {
        snprintf(destpath, sizeof(destpath), "%s", cur_name);
      } else if (dest_is_dir) {
        size_t dl = strlen(dest_arg);
        if (dl > 0 && dest_arg[dl - 1] == '/') {
          snprintf(destpath, sizeof(destpath), "%s%s", dest_arg, cur_name);
        } else {
          snprintf(destpath, sizeof(destpath), "%s/%s", dest_arg, cur_name);
        }
      } else if (!single_used) {
        single_used = 1;
        snprintf(destpath, sizeof(destpath), "%s", dest_arg);
      } else {
        discarding = 1;
        fprintf(stderr,
          "Ignoring additional file(s) sent by host (single-file mode).\n");
      }

      if (!discarding) {
        out = fopen(destpath, "wb");
        if (!out) {
          fprintf(stderr, "Cannot create %s: %s\n", destpath,
            strerror(errno));
          discarding = 1;
          any_err++;
        } else if (verbose) {
          fprintf(stderr, "Receiving %s (%lu bytes)...\n", destpath,
            (unsigned long)cur_size);
        }
      }

      /* header's payload ack -- sent only now that we're genuinely
       * ready for data (file opened, or a decision to discard has
       * already been made on every path above) */
      if (send_chunk_ack() < 0) { fatal = 1; break; }

      expecting_header = 0;
    } else {
      /* data chunk -- credited/reported here, not in recv_chunk(),
       * so a header's own payload bytes never show up as a
       * "Kbytes transferred" line (see recv_chunk()'s own comment) */
      bdone += len;
      if (verbose) stats();

      if (!discarding && out) {
        if (fwrite(buf, 1, len, out) != len) {
          fprintf(stderr, "%s: write error\n", destpath);
          fclose(out);
          out = NULL;
          fatal = 1;
          break;                /* no ack -- fatal, matches mr.asm's
                                  * own identical policy for this exact
                                  * case */
        }
      }

      /* ack AFTER the write completes (or was skipped while
       * discarding) -- the whole point of this design: the sender
       * genuinely waits for us to be done, not just for the wire read
       * to finish */
      if (send_chunk_ack() < 0) { fatal = 1; break; }
    }
  }

  if (out) fclose(out);

  if (verbose) {
    fprintf(stderr, "\n%d file(s) received", any_ok);
    if (any_err) fprintf(stderr, ", %d failed", any_err);
    fprintf(stderr, ".\n");
  }

  return (fatal || any_err) ? -1 : 0;
}

static void usage(void)
{
  fprintf(stderr, "\
Usage: max-xfr -s [-v] [-d <delay>] <file> [file...]\n\
       max-xfr -r [-v] [-d <delay>] [<destination>]\n\
       -s:  send one or more files (batch mode)\n\
       -r:  receive whatever the sender offers, into <destination> if\n\
            given (an existing directory selects batch mode into it;\n\
            any other name selects single-file mode, saving only the\n\
            first file offered), or into the current directory\n\
            otherwise\n\
       -v:  verbose (statistics and per-file progress on stderr)\n\
       -d:  delay in microseconds between bytes while sending, and\n\
            before each ack byte while receiving (needed on some\n\
            bit-banged links -- see the ack-pacing comments in\n\
            send_chunk_ack()/recv_chunk() for why)\n");
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

  /* control chars - set return condition: min number of bytes and timer */
  raw.c_cc[VMIN] = 5; raw.c_cc[VTIME] = 8; /* after 5 bytes or .8 seconds
                                              after first byte seen      */
  raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0; /* immediate - anything       */
  raw.c_cc[VMIN] = 2; raw.c_cc[VTIME] = 0; /* after two bytes, no timer  */
  /* after a byte, or 25.5 seconds (VTIME's own 1-byte max -- tenths of
   * a second) with none: was VTIME=8 (0.8s) until 2026-09-05. Real
   * confirmed need: max-xfr -r writes its own $AA and immediately
   * blocks waiting for the far end's $55 sync byte -- the user has to
   * physically switch to the OTHER machine and start ms/max-xfr -s
   * there in between, and 0.8s isn't much time for that. (An earlier
   * attempt at this same change was reverted the same day over a
   * DIFFERENT, unrelated failure that turned out to be a disconnected
   * bit-bang RX line, not a timing issue at all -- this reapplies it
   * for the real, confirmed reason.) This setting applies to every
   * read() for the life of the process, not just the initial
   * handshake, so there's no correctness reason to keep it tight -- a
   * genuine failure (nothing ever responds) still eventually times out
   * and is reported accurately via read_expected_byte(), it just takes
   * longer to notice. */
  raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 255;

  /* put terminal in raw mode after flushing */
  if (tcsetattr(ttyfd,TCSAFLUSH,&raw) < 0) fatal("can't set raw mode");

  raw_mode++;
}

int main(int argc, char **argv)
{
  int c;
  int what = 0;
  int ret;
  uint8_t over = 'x';

  while ((c = getopt(argc, argv, "d:rsv")) != EOF) {
    switch (c) {
      case 'd':
        delay = atoi(optarg);
        break;
      case 's':
      case 'r':
        what = c;
        break;
      case 'v':
        verbose++;
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
  if (what == 's' && optind >= argc) usage();       /* need >= 1 file */
  if (what == 'r' && argc - optind > 1) usage();     /* 0 or 1 dest */

  /* check that input is from a tty */
  if (! isatty(ttyfd)) fatal("not on a tty");

  /* store current tty settings in orig_termios */
  if (tcgetattr(ttyfd,&orig_termios) < 0) fatal("can't get tty settings");

  /* register the tty reset with the exit handler */
  if (atexit(tty_atexit) != 0) fatal("atexit: can't register tty reset");

  tty_raw();      /* put tty in raw mode */

  if (what == 's') {
    int nfiles = argc - optind;
    if (verbose) {
      fprintf(stderr, _("Sending %d file(s)\n\n"), nfiles);
      fflush(stderr);
    }
    ret = send_batch(argv + optind, nfiles);
  } else {
    const char *dest_arg = (argc - optind == 1) ? argv[optind] : NULL;
    int dest_is_dir = 0;

    if (dest_arg != NULL) {
      struct stat st;
      if (stat(dest_arg, &st) == 0 && S_ISDIR(st.st_mode)) {
        dest_is_dir = 1;
      }
    }

    if (verbose) {
      fprintf(stderr, _("Receiving into %s\n\n"),
        dest_arg ? dest_arg : "the current directory");
      fflush(stderr);
    }
    ret = receive_batch(dest_arg, dest_is_dir);
  }

  tty_reset();

  write(STDOUT_FILENO, &over, 1);

  if (verbose) {
    fprintf(stderr, _("... Done.\n"));
    fflush(stderr);
  }

  return ret < 0 ? 1 : 0;
}
