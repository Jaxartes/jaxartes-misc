/* timedumper.c - Jeremy Dilatush
 *
 * Copyright (c) 2013 - 2020, Jeremy Dilatush
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY JEREMY DILATUSH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL JEREMY DILATUSH OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * timedumper - This program dumps some stuff to standard output.
 * End it with control-C.
 */

#include <stdio.h>

/* Early history of this software:
 * 2-5 June 2013 - initial
 * 27 Jul 2013 - enhanced with option -c for color
 * 4 Aug 2014 - enhanced with option -q for approximately quarter duty cycle
 * 25 Feb 2019 - took out extra whitespace, added longer LFSR, changed
 * time format.
 * 16 Jul 2019 - replaced 64 bit LFSR with (pseudo)random one for
 * better mixing
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

int main(int argc, char *argv[])
{
  unsigned long long ctr = 0;
  struct timeval tv;
  char ctb[32];
  unsigned lfsr = 1;
  unsigned long long lfsr64 = 1;
  int copt = 0, qopt = 0;

  if (argc > 0) {
    --argc;
    ++argv;
  }
  while (argc > 0) {
    if (!strcmp(argv[0], "-c")) copt = 1;
    else if (!strcmp(argv[0], "-q")) qopt = 1;
    else break;
    --argc;
    ++argv;
  }
  if (argc > 1 && !strcmp(argv[1], "-c")) {
    copt = 1;
  }

  for (;;) {
    memset(&tv, 0, sizeof tv);
    memset(ctb, 0, sizeof ctb);
    gettimeofday(&tv, NULL);
    if (qopt) {
      unsigned in100ms = tv.tv_usec % 100000;
      if (in100ms >= 25000) {
        /* save CPU time by skipping the remaining 75ms */
        usleep(100000 - in100ms);
        gettimeofday(&tv, NULL);
      }      
    }
    strftime(ctb, sizeof(ctb), "%Y-%m-%d-%H:%M:%S", localtime(&(tv.tv_sec)));
    if (copt) {
      printf("\033[3%cm\033[4%cm",
	     (int)('0' + (lfsr & 7)), (int)('0' + ((lfsr >> 3) & 7)));
    }
    printf("%15llu   %s.%06u   %06x   %016llx\n",
	   (unsigned long long)ctr, ctb,
	   (unsigned)tv.tv_usec, (unsigned)lfsr, (unsigned long long)lfsr64);
    if (copt) {
      printf("\033[m");
    }
    fflush(stdout);

    ++ctr;
    lfsr <<= 1; if (lfsr & 0x1000000) lfsr ^= 0x1864CFB;
    if (lfsr64 & 0x8000000000000000ull) {
        lfsr64 = (lfsr64 << 1) ^ 0x33de9a5ec699abb1ull;
    } else {
        lfsr64 <<= 1;
    }
    lfsr64 &= 0xffffffffffffffffull;
  }
}
