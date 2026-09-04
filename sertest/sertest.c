/*
 * sertest.c - host-side companion to ELF-DOS's test/sertest.asm
 *
 * A minimal serial-connectivity diagnostic, matching the ELF-DOS side's
 * own tool byte-for-byte in spirit: no protocol beyond "send a string
 * plus CR LF" / "read bytes until CR, LF, or a length cap, echoing
 * each one to the console as it arrives". Run one end on the Pi, the
 * other on ELF-DOS (SERTEST -u|-b -s/-r), to test raw wiring/baud
 * independent of MR/MS/YR/YS's own batch protocol entirely.
 *
 * Invoked the same way as max-xfr: stdin/stdout redirected to the
 * serial device, e.g.
 *   sertest -s -d 1000 hello < /dev/ttyAMA2 > /dev/ttyAMA2
 *   sertest -r            < /dev/ttyAMA2 > /dev/ttyAMA2
 * (stdout is only actually written by -s; -r never writes to the
 * wire at all, all of its own output goes to stderr, matching
 * max-xfr's own "stdout is the wire, human-readable output goes to
 * stderr" convention -- keeps a listener from ever polluting the
 * link with its own diagnostic text.)
 *
 * -d <delay>: microseconds to sleep between each byte written during
 * -s -- same option, same semantics, same default as max-xfr's own
 * -d. Needed because a plain back-to-back byte stream can outrun what
 * the OTHER end can actually keep up with, especially a bit-banged
 * (software) UART at higher baud rates -- confirmed necessary in
 * practice: minicom's own inter-character delay set to 1ms is what
 * let TERMSIZE and arrow-key input work at 38400 baud over the ELF-
 * DOS bit-bang port. No delay was needed for the same two cases over
 * the hardware UART, but a small one may still matter for MR at the
 * highest baud rates even there.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <termios.h>

#define RECV_MAX        127     /* matches ELF-DOS's own SERTEST_MAX */
#define DEFAULT_DELAY   130     /* matches max-xfr's own default */

static int delay = DEFAULT_DELAY;

static struct termios orig_termios;
static int raw_mode = 0;
static int ttyfd = STDIN_FILENO;

/*
 *      write_all: write() with retry on a short write or EINTR.
 */
static int write_all(const void *buf, size_t len)
{
  const uint8_t *p = buf;
  while (len) {
    ssize_t ret = write(STDOUT_FILENO, p, len);
    if (ret < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, "Error while writing (errno = %d)\n", errno);
      return -1;
    }
    p += ret;
    len -= (size_t)ret;
  }
  return 0;
}

static void print_hex_byte(uint8_t b)
{
  static const char digits[] = "0123456789ABCDEF";
  fputc(digits[(b >> 4) & 0xF], stderr);
  fputc(digits[b & 0xF], stderr);
}

/*
 *      do_send: write str's raw bytes, each followed by a "-d"
 *      microsecond delay, then CR LF.
 */
static int do_send(const char *str)
{
  size_t i, len = strlen(str);
  uint8_t b;

  for (i = 0; i < len; i++) {
    b = (uint8_t)str[i];
    if (write_all(&b, 1) < 0) return -1;
    if (delay) usleep(delay);
  }

  b = 13;
  if (write_all(&b, 1) < 0) return -1;
  if (delay) usleep(delay);
  b = 10;
  if (write_all(&b, 1) < 0) return -1;
  if (delay) usleep(delay);

  fprintf(stderr, "Sent.\n");
  return 0;
}

/*
 *      do_recv: block reading one byte at a time, echoing each to
 *      stderr (raw if printable ASCII, "[XX]" hex otherwise), until
 *      CR, LF, or RECV_MAX bytes.
 */
static int do_recv(void)
{
  uint8_t b;
  int count = 0;

  fprintf(stderr, "Waiting...\n");

  for (;;) {
    ssize_t ret = read(STDIN_FILENO, &b, 1);
    if (ret != 1) {
      fprintf(stderr, "\nRead error (errno = %d)\n", errno);
      return -1;
    }

    if (b == 13) {
      fprintf(stderr, "<CR>");
      break;
    }
    if (b == 10) {
      fprintf(stderr, "<LF>");
      break;
    }

    if (b >= 0x20 && b < 0x7F) {
      fputc(b, stderr);
    } else {
      fputc('[', stderr);
      print_hex_byte(b);
      fputc(']', stderr);
    }

    count++;
    if (count >= RECV_MAX) {
      fprintf(stderr, "<full>");
      break;
    }
  }

  fprintf(stderr, "\nDone.\n");
  return 0;
}

static void usage(void)
{
  fprintf(stderr, "\
Usage: sertest -s [-d <delay>] <string>\n\
       sertest -r\n\
       -s:  send <string> (+ CR LF) out the port\n\
       -r:  read from the port, echoing to stderr until CR/LF/%d bytes\n\
       -d:  delay in microseconds between sent bytes (default %d --\n\
            a larger value, e.g. 1000, may be needed at high baud,\n\
            especially with a bit-banged UART on the other end)\n",
    RECV_MAX, DEFAULT_DELAY);
  exit(1);
}

static void fatal(const char *message)
{
  fprintf(stderr, "fatal error: %s\n", message);
  exit(1);
}

static int tty_reset(void)
{
  if (tcsetattr(ttyfd, TCSAFLUSH, &orig_termios) == 0) {
    raw_mode = 0;
    return 0;
  }
  return -1;
}

static void tty_atexit(void)
{
  if (raw_mode) tty_reset();
}

static void tty_raw(void)
{
  struct termios raw;

  raw = orig_termios;

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  /* VMIN=1, VTIME=0: block until exactly one byte is available, no
   * timeout -- matches do_recv's own "one byte per read() call"
   * convention exactly. */
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(ttyfd, TCSAFLUSH, &raw) < 0) fatal("can't set raw mode");

  raw_mode++;
}

int main(int argc, char **argv)
{
  int c;
  int what = 0;
  const char *str = NULL;
  int ret;

  while ((c = getopt(argc, argv, "d:rs")) != EOF) {
    switch (c) {
      case 'd':
        delay = atoi(optarg);
        break;
      case 's':
      case 'r':
        what = c;
        break;
      case '?':
        usage();
        break;
      default:
        usage();
        break;
    }
  }

  if (what == 0) usage();
  if (what == 's') {
    if (optind >= argc) usage();
    str = argv[optind];
  }

  if (!isatty(ttyfd)) fatal("not on a tty");
  if (tcgetattr(ttyfd, &orig_termios) < 0) fatal("can't get tty settings");
  if (atexit(tty_atexit) != 0) fatal("atexit: can't register tty reset");

  tty_raw();

  if (what == 's') {
    ret = do_send(str);
  } else {
    ret = do_recv();
  }

  tty_reset();

  return ret < 0 ? 1 : 0;
}
