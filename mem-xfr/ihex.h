/* $Id$ */

#ifndef IHEX_H
#define IHEX_H

#include <stdio.h>

/* parse a line of intel hex */
int parse_hex_line(const char *theline, int bytes[], int *addr, int *num,
  int *code);

/* this does the dirty work of writing an intel hex file */
/* caution, static buffering is used, so it is necessary */
/* to call it with end=1 when finished to flush the buffer */
/* and close the file */
void hexout(FILE *fhex, int byte, int memory_location, int end);

#endif
