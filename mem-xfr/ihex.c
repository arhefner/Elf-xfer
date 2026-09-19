/* Intel HEX read/write functions, Paul Stoffregen, paul@ece.orst.edu */
/* This code is in the public domain.  Please retain my name and */
/* email address in distributed copies, and let me know about any bugs */

/* I, Paul Stoffregen, give no warranty, expressed or implied for */
/* this software and/or documentation provided, including, without */
/* limitation, warranty of merchantability and fitness for a */
/* particular purpose. */

/* Recovered (2026-09-19) from max-xfr's own history -- these two files
 * were removed from max-xfr in commit 6ad5007 when its Intel-HEX mode was
 * dropped, on the grounds that BIOS-level hex/binary memory-image
 * transfers were "getting their own separate, dedicated utility instead".
 * mem-xfr is that utility, so the code comes back here essentially
 * unchanged. The only edits are mechanical: the old K&R-style function
 * definitions are now ANSI prototypes (gcc 13 still accepts K&R here,
 * but warns under -Wall, and C23 removes the form outright), theline is
 * const since parse_hex_line never writes through it, and hexout no
 * longer touches its static buffer after the end=1 call has already
 * closed the file. */

#include <stdio.h>
#include <string.h>
#include "ihex.h"

/* parses a line of intel hex code, stores the data in bytes[] */
/* and the beginning address in addr, and returns a 1 if the */
/* line was valid, or a 0 if an error occured.  The variable */
/* num gets the number of bytes that were stored into bytes[] */

int parse_hex_line(const char *theline, int bytes[], int *addr, int *num,
  int *code)
{
	int sum, len, cksum;
	const char *ptr;

	*num = 0;
	if (theline[0] != ':') return 0;
	if (strlen(theline) < 11) return 0;
	ptr = theline+1;
	if (!sscanf(ptr, "%02x", &len)) return 0;
	ptr += 2;
	if ( strlen(theline) < (size_t)(11 + (len * 2)) ) return 0;
	if (!sscanf(ptr, "%04x", addr)) return 0;
	ptr += 4;
	  /* printf("Line: length=%d Addr=%d\n", len, *addr); */
	if (!sscanf(ptr, "%02x", code)) return 0;
	ptr += 2;
	sum = (len & 255) + ((*addr >> 8) & 255) + (*addr & 255) + (*code & 255);
	while(*num != len) {
		if (!sscanf(ptr, "%02x", &bytes[*num])) return 0;
		ptr += 2;
		sum += bytes[*num] & 255;
		(*num)++;
		if (*num >= 256) return 0;
	}
	if (!sscanf(ptr, "%02x", &cksum)) return 0;
	if ( ((sum & 255) + (cksum & 255)) & 255 ) return 0; /* checksum error */
	return 1;
}

/* produce intel hex file output... call this routine with */
/* each byte to output and it's memory location.  The file */
/* pointer fhex must have been opened for writing.  After */
/* all data is written, call with end=1 (normally set to 0) */
/* so it will flush the data from its static buffer */

#define MAXHEXLINE 32	/* the maximum number of bytes to put in one line */

void hexout(FILE *fhex, int byte, int memory_location, int end)
{
	static int byte_buffer[MAXHEXLINE];
	static int last_mem, buffer_pos, buffer_addr;
	static int writing_in_progress=0;
	register int i, sum;

	if (!writing_in_progress) {
		/* initial condition setup */
		last_mem = memory_location-1;
		buffer_pos = 0;
		buffer_addr = memory_location;
		writing_in_progress = 1;
		}

	if ( (memory_location != (last_mem+1)) || (buffer_pos >= MAXHEXLINE) \
	 || ((end) && (buffer_pos > 0)) ) {
		/* it's time to dump the buffer to a line in the file */
		fprintf(fhex, ":%02X%04X00", buffer_pos, buffer_addr);
		sum = buffer_pos + ((buffer_addr>>8)&255) + (buffer_addr&255);
		for (i=0; i < buffer_pos; i++) {
			fprintf(fhex, "%02X", byte_buffer[i]&255);
			sum += byte_buffer[i]&255;
		}
		fprintf(fhex, "%02X\n", (-sum)&255);
		buffer_addr = memory_location;
		buffer_pos = 0;
	}

	if (end) {
		fprintf(fhex, ":00000001FF\n");  /* end of file marker */
		fclose(fhex);
		writing_in_progress = 0;
		return;                 /* fhex is closed -- don't buffer past it */
	}

	last_mem = memory_location;
	byte_buffer[buffer_pos] = byte & 255;
	buffer_pos++;
}
