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
#include <getopt.h>
#include <termios.h>

#include "intl.h"
#include "ihex.h"

uint8_t memory[65536];

typedef struct chunk
{
  uint16_t      address;
  size_t        size;
  struct chunk *next;
} Chunk;

Chunk *chunks = NULL;
Chunk *tail = NULL;

/*
 *	Externals.
 */
extern int optind;
extern char *optarg;

/*
 *	Global variables.
 */
static int verbose = 0;
static int raw_mode = 0;
static struct timespec start;
static unsigned long bdone = 0;

static struct termios orig_termios;  /* TERMinal I/O Structure */
static int ttyfd = STDIN_FILENO;     /* STDIN_FILENO is 0 by default */

static int delay = 130;              /* Default value for 57.6k hw UART */


static void add_chunk(Chunk *chunk)
{
  chunk->next = NULL;

  if (tail)
  {
    tail->next = chunk;
  }
  else
  {
    chunks = chunk;
  }

  tail = chunk;
}

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

const char *get_file_extension(const char *path)
{
    const char *result;
    int i, n;

    assert(path != NULL);
    n = strlen(path);
    i = n - 1;
    while ((i > 0) && (path[i] != '.') && (path[i] != '/') && (path[i] != '\\')) {
        i--;
    }
    if ((i > 0) && (i < n - 1) && (path[i] == '.') && (path[i - 1] != '/') && (path[i - 1] != '\\')) {
        result = path + i;
    } else {
        result = path + n;
    }
    return result;
}

static int read_bin(const char *file)
{
  FILE *fp;
  uint16_t address = 0;

  if ((fp = fopen(file, "rb")) == NULL) {
    perror(file);
    return -1;
  }

  Chunk *the_chunk = (Chunk *)malloc(sizeof(Chunk));
  the_chunk->address = address;
  the_chunk->next = NULL;

  fseek(fp, 0L, SEEK_END);
  the_chunk->size = ftell(fp);

  fseek(fp, 0L, SEEK_SET);
  fread(memory+address, 1, the_chunk->size, fp);

  chunks = tail = the_chunk;

  fclose(fp);

  return 0;
}

static int read_hex(const char *file)
{
  FILE *fp;
  char line[1024];
  int hex_addr, n, status, bytes[256];
  int i, lineno = 1;
  int address = 0;
  int first = 1;

  Chunk *the_chunk = (Chunk *)malloc(sizeof(Chunk));
  the_chunk->address = address;
  the_chunk->size = 0;
  the_chunk->next = NULL;

  if ((fp = fopen(file, "r")) == NULL) {
    perror(file);
    return -1;
  }

  while (fgets(line, sizeof(line) - 2, fp) != NULL)
  {
    if (parse_hex_line(line, bytes, &hex_addr, &n, &status))
    {
      if (status == 0)
      {  /* data */
        if (hex_addr != the_chunk->address + the_chunk->size)
        {
          if (the_chunk->size != 0)
          {
            add_chunk(the_chunk);

            the_chunk = (Chunk *)malloc(sizeof(Chunk));
            the_chunk->size = 0;
            the_chunk->next = NULL;
          }

          the_chunk->address = hex_addr;
        }

        for (int i = 0; i < n; i++)
        {
          memory[hex_addr + i] = bytes[i] & 0xff;
        }

        the_chunk->size += n;
      }
      else if (status == 1)
      {  /* end of file */
        add_chunk(the_chunk);
        fclose(fp);
        return 0;
      }
    }
    else
    {
      fprintf(stderr, "Error parsing hex file.");
      return -1;
    }
  }

  fclose(fp);
  return 0;
}

static int send()
{
  Chunk *chunk;
  uint8_t end = 0x00;
  uint8_t ack = 0x55;
  ssize_t ret;

  clock_gettime(CLOCK_MONOTONIC, &start);

  ret = write(STDOUT_FILENO, &ack, 1);
  if (ret != 1) {
    fprintf(stderr, _("Error while writing (errno = %d)\n"), errno);
    return -1;
  }

  ack = getchar();

  if (ack != 0xaa)
  {
    fprintf(stderr, "Invalid handshake: %02x.\n",ack);
    return -1;
  }

  for (chunk = chunks; chunk != NULL; chunk = chunk->next)
  {
    uint16_t address = chunk->address;
    size_t remaining = chunk->size;

    while (remaining)
    {
      size_t count = (remaining > 512) ? 512 : remaining;
      uint8_t *output = memory + address;
      uint8_t cmd[5];
      uint8_t ack;

      cmd[0] = 0x01;
      cmd[1] = (uint8_t)((count >> 8) & 0xff);
      cmd[2] = (uint8_t)(count & 0xff);
      cmd[3] = (uint8_t)((address >> 8) & 0xff);
      cmd[4] = (uint8_t)(address & 0xff);

      for (int i = 0; i < sizeof(cmd); i++)
      {
        write(STDOUT_FILENO, cmd+i, 1);
        ack = getchar();
        if (ack != *(cmd+i))
        {
          fprintf(stderr, "Error while writing (cmd = %02x, ack = %02x)\n",
            *(cmd+i), ack);
          return -1;
        }
      }

      for (int i = 0; i < count; i++)
      {
        write(STDOUT_FILENO, output++, 1);
        usleep(delay);
      }

      address += count;
      remaining -= count;
      bdone += count;

      if (verbose)
      {
        stats();
      }

      ack = getchar();
      if (ack != 0xaa)
      {
          fprintf(stderr, "Error in ack (ack = %02x)\n", ack);
          return -1;
      }
    }
  }

  write(STDOUT_FILENO, &end, 1);

  return 0;
}

static int write_bin(const char *file)
{
  Chunk *chunk;
  FILE *fp;

  if ((fp = fopen(file, "wb")) == NULL) {
    perror(file);
    return -1;
  }

  for (chunk = chunks; chunk != NULL; chunk = chunk->next)
  {
    fwrite(memory+chunk->address, 1, chunk->size, fp);
  }

  fclose(fp);

  return 0;
}

static int write_hex(const char *file)
{
  Chunk *chunk;
  FILE *fp;

  if ((fp = fopen(file, "w")) == NULL) {
    perror(file);
    return -1;
  }

  for (chunk = chunks; chunk != NULL; chunk = chunk->next)
  {
    int addr = chunk->address;

    for (int i = 0; i < chunk->size; i++)
    {
      hexout(fp, memory[addr], addr, 0);
      addr++;
    }
  }

  hexout(fp, 0, 0, 1);

  return 0;
}

static int receive()
{
  Chunk *a_chunk;
  uint8_t in;
  uint16_t address;
  uint16_t count;
  uint8_t ack = 0xaa;
  ssize_t ret;
  ssize_t remaining;
  uint8_t *mem;

  clock_gettime(CLOCK_MONOTONIC, &start);

  ret = write(STDOUT_FILENO, &ack, 1);
  if (ret != 1) {
    fprintf(stderr, _("Write error (errno = %d)\n"), errno);
    return -1;
  }

  read(STDIN_FILENO, &ack, 1);

  if (ack != 0x55)
  {
    fprintf(stderr, "Invalid handshake: %02x\n", ack);
    return -1;
  }

  a_chunk = (Chunk *)malloc(sizeof(Chunk));
  a_chunk->address = 0;
  a_chunk->size = 0;

  read(STDIN_FILENO, &in, 1);

  while (in != 0x00)
  {
    if (in != 0x01)
    {
      fprintf(stderr, "Invalid command: %02x\n", in);
      return -1;
    }

    write(STDOUT_FILENO, &in, 1);

    read(STDIN_FILENO, &in, 1);

    count = in;
    write(STDOUT_FILENO, &in, 1);

    read(STDIN_FILENO, &in, 1);

    count = (count << 8) | in;
    write(STDOUT_FILENO, &in, 1);

    read(STDIN_FILENO, &in, 1);

    address = in;
    write(STDOUT_FILENO, &in, 1);

    read(STDIN_FILENO, &in, 1);

    address = (address << 8) | in;
    write(STDOUT_FILENO, &in, 1);

    mem = memory + address;
    remaining = count;

    while (remaining)
    {
      ret = read(STDIN_FILENO, mem, remaining);

      if (ret < 0)
      {
        fprintf(stderr, _("Read error (errno = %d)\n"), errno);
        return -1;
      }
      mem += ret;
      remaining -= ret;
    }

    usleep(delay);

    if (address != a_chunk->address + a_chunk->size)
    {
      if (a_chunk->size != 0)
      {
        add_chunk(a_chunk);

        a_chunk = (Chunk *)malloc(sizeof(Chunk));
      }

      a_chunk->address = address;
      a_chunk->size = count;
    }
    else
    {
      a_chunk->size += count;
    }

    bdone += count;

    if (verbose)
    {
      stats();
    }

    ack = 0xaa;
    write(STDOUT_FILENO, &ack, 1);

    ret = read(STDIN_FILENO, &in, 1);
  }

  add_chunk(a_chunk);

  fflush(stderr);

  return 0;
}

static int is_hex(const char *file)
{
  const char *ext = get_file_extension(file);

  return ((strcmp(ext, ".hex") == 0) || (strcmp(ext, ".intel") == 0));
}

/*
 *	Send a file in MAX mode.
 */
static int msend(const char *file)
{
  int ret;

  if (is_hex(file))
  {
    ret = read_hex(file);
  }
  else
  {
    ret = read_bin(file);
  }

  if (ret == 0)
  {
    ret = send();
  }

  return ret;
}

/*
 *	Receive a file in MAX mode.
 */
static int mrecv(const char *file)
{
  int ret;

  ret = receive();

  if (ret == 0)
  {
    if (is_hex(file))
    {
      ret = write_hex(file);
    }
    else
    {
      ret = write_bin(file);
    }
  }

  return ret;
}

static void usage(void)
{
  fprintf(stderr, "\
Usage: max-xfr -s|-r [-v] [-d <delay>] filename\n\
       -s:  send\n\
       -r:  receive\n\
       -v:  verbose (statistics on stderr output)\n\
       -d:  delay in microseconds for send\n");
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
  raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 8; /* after a byte or .8 seconds */

  /* put terminal in raw mode after flushing */
  if (tcsetattr(ttyfd,TCSAFLUSH,&raw) < 0) fatal("can't set raw mode");

  raw_mode++;
}

int main(int argc, char **argv)
{
  int c;
  int what = 0;
  char *file;
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

  if (optind != argc - 1 || what == 0)
    usage();
  file = argv[optind];

  /* check that input is from a tty */
  if (! isatty(ttyfd)) fatal("not on a tty");

  /* store current tty settings in orig_termios */
  if (tcgetattr(ttyfd,&orig_termios) < 0) fatal("can't get tty settings");

  /* register the tty reset with the exit handler */
  if (atexit(tty_atexit) != 0) fatal("atexit: can't register tty reset");

  tty_raw();      /* put tty in raw mode */

  if (what == 's') {
    fprintf(stderr, _("Download of \"%s\"\n\n"), file);
    fflush(stderr);
    ret = msend(file);
  } else {
    fprintf(stderr, _("Upload of \"%s\"\n\n"), file);
    fflush(stderr);
    ret = mrecv(file);
  }

  tty_reset();

  write(STDOUT_FILENO, &over, 1);

  if (verbose) {
    fprintf(stderr, _("... Done.\n"));
    fflush(stderr);
  }

  return ret < 0 ? 1 : 0;
}
