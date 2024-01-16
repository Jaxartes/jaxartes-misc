/* stdserve.c
 * Copyright (C) 2011, Jeremy Dilatush.
 * Use under a BSD license. Use with caution. See below.
 * 2011/Dec/03
 *
 * "stdserve" is a server for a handful of simple protocols on the internet.
 * These protocols were never very useful, and are not usually used nowadays.
 * But they have some value for testing:  These days I work on a firewall, and
 * for debugging it I need to be able to create connections.
 *
 * Most of these protocols are supported by the various implementations of
 * 'inetd'.  But the reason I'm writing a separate one is because of a
 * limitation of 'inetd': Starting a new process for each connection.
 *
 * This program will handle several connections in a single process before
 * starting a new one.  That should allow a single node to handle more
 * connections than otherwise.
 *
 * Written in 2011, and never quite finished. Don't run on an important
 * production machine, or anywhere this software's bugs might
 * unacceptably impact security or utility. But for tests and experiments
 * it might be good enough.
 */
/*
 * stdserve.c - Jeremy Dilatush
 *
 * Copyright (C) 2011, Jeremy Dilatush.
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
 */

#define DO_IPv6
/* #undef HAS_AI_NUMERICSERV */
#define WAITPID_MINUS_ONE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include <netdb.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>

static void usage(void)
{
  fputs("Command line SYNTAX of stdserve:\n"
	"\tstdserve [$opts] $proto [$addr...]\n"
	"\t$opts - options for stdserve\n"
	"\t\t-N num - number of connections per process; default 100; 0 unlimited\n"
	"\t\t-v - verbose output\n"
#ifdef DO_IPv6
	"\t\t-6 - do IPv6 instead of IPv4\n"
#endif
	"\t\t-n - no lookups of addresses/ports; only use numeric ones\n"
	"\t$proto - protocol to use\n"
	"\t\techo - RFC 862 protocol; default port 7\n"
	"\t\tdiscard - RFC 863 protocol; default port 9\n"
	"\t\tdaytime - RFC 867 protocol; default port 13\n"
	"\t\ttime - RFC 868 protocol; default port 37\n"
	"\t\tchargen - RFC 864 protocol; default port 19\n"
	"\t\tqotd - RFC 865 protocol; default port 17\n"
	"\t\t\tinstead of a quote, generates a pseudorandom word sequence.\n"
	"\t\t\ttakes some optional parameters:\n"
	"\t\t\t\t-d $dictfile - dictionary file\n"
	"\t\t\t\t-w $nwords - number of words (default 5)\n"
	"\t\t\t\t-w $min-$max - range of values for number of words\n"
	"\t\tgen - generates traffic in the form of brief informational\n"
	"\t\t\tmessages, issued at intervals; optional parameters:\n"
	"\t\t\t\t-i $sec - interval in seconds between messages (1 sec)\n"
	"\t\t\t\t-r $sec - additional amount to \"randomize\" interval (0 sec)\n"
	"\t\t\t\t-n $msgs - number of messages before terminating (0 = inf)\n"
	"\t\t\t\t-d $sec - delay before terminating (0 = none)\n"
	"\t$addr - optionally, one or more addresses/ports\n"
	"\t\tIf none specified, uses default.\n"
	"\t\tMay take the following forms:\n"
	"\t\t\taddress - numeric or name IP (or IPv6) address\n"
	"\t\t\t/port - port number or service name\n"
	"\t\t\taddress/port - both\n",
	stderr);
  exit(1);
}

struct {
  /* global parameters */
  int verbose;
  unsigned long long verbose_extra;
  int conns_per_proc;
#ifdef DO_IPv6
  int ipv6;
#endif
  int numeric;
  int sigusr2_pending;
} gparm;

#define VERBOSE_EXTRA_BIT(c) (1ULL << (c & 63))
#define MAYBE_VERBOSE(l,c) (gparm.verbose >= l || (gparm.verbose_extra & VERBOSE_EXTRA_BIT(c)))

static long long usnow; /* time in microseconds since the epoch (1970) */

static void backoff_delay(int magnitude);

static void update_usnow(void)
{
  /* figure out what time it is now */
  struct timeval tv;
  if (gettimeofday(&tv, NULL) < 0) {
    perror("gettimeofday");
    backoff_delay(1);
  }
  usnow = tv.tv_sec;
  usnow = usnow * 1000000 + tv.tv_usec;
  if (MAYBE_VERBOSE(2,'u')) {
    fprintf(stderr, "usnow=%lld\n", (long long)usnow);
  }
}

#define BACKOFF_USEC_INITIAL 1000
static long long backoff_usec = BACKOFF_USEC_INITIAL;
static void backoff_delay(int magnitude)
{
  long long usold = usnow, sleepfor, elapsed;
  update_usnow();
  elapsed = usnow - usold;
  if (magnitude < 1) {
    if (elapsed > (backoff_usec * 10 + 1000000)) {
      backoff_usec = BACKOFF_USEC_INITIAL;
    }
    sleepfor = backoff_usec;
    backoff_usec += 1 + (backoff_usec >> 2);
  } else {
    if (elapsed > (backoff_usec * 4 + 250000)) {
      backoff_usec = BACKOFF_USEC_INITIAL;
    }
    sleepfor = backoff_usec;
    if (sleepfor > 100000) {
      sleepfor = 100000;
    }
    backoff_usec += 1 + (backoff_usec >> 3);
  }
  if (backoff_usec > 250000) {
    backoff_usec = 250000;
  }
  if (MAYBE_VERBOSE(1,'b')) {
    fprintf(stderr, "Backoff delay %lld usec\n", (long long)backoff_usec);
  }
  usleep(sleepfor);
}

#define New(v) v=malloc(sizeof(*(v)));if(!v){perror("memory management failure");exit(2);};memset(v,0,sizeof(*(v)))

static struct {
  /* State for the *rand48() pseudo random number generator.
   *	xsubi - the current state
   *	branch - state for when a new process gets forked
   */
  unsigned short xsubi[3];
  unsigned short branch[9];
} prng;

static void prngmunge(void)
{
  int i;
  unsigned short a;
  static unsigned short rotor[16] = {
    /* from pi */
    0x1243, 0x2F6A, 0x3888, 0x45A3, 0x508D, 0x6313, 0x7198, 0x8A2E,
    0x9037, 0xa734, 0xb4A4, 0xc093, 0xd822, 0xe299, 0xfF31, 0x0D00
  };

  for (i = 0; i < 45; ++i) {
    a = prng.branch[i % 9];
    a = ((a >> 4) & 4095) ^ rotor[a & 15];
    prng.branch[(i + 1) % 9] ^= a;
  }
}

static void prngseed_dumb(void)
{
  /* This will fill in the PRNG state with some usable if not great values,
   * in case prngseed_smart() fails.
   */
  int i;
  long long tstart, ctr;

  update_usnow();
  tstart = usnow;

  for (ctr = 0; ; ) {
    i = gparm.verbose;
    gparm.verbose = 0;
    update_usnow();
    gparm.verbose = i;
    if (usnow < tstart || usnow >= tstart + 10000) {
      break;
    }
    ++ctr;
  }

  prng.branch[0] = getpid();
  prng.branch[1] = usnow;
  prng.branch[2] = usnow >> 16;
  prng.branch[3] = usnow >> 32;
  prng.branch[4] = usnow >> 48;
  prng.branch[5] = ctr;
  prng.branch[6] = 25814;
  prng.branch[7] = 36925;
  prng.branch[8] = 47036;

  if (gparm.verbose > 1) {
    for (i = 0; i < 9; ++i) {
      fprintf(stderr, "prngseed_dumb() pre-munge prng.branch[%d]=%hu\n",
	      i, prng.branch[i]);
    }
  }
  prngmunge();
  if (gparm.verbose > 1) {
    for (i = 0; i < 9; ++i) {
      fprintf(stderr, "prngseed_dumb() post-munge prng.branch[%d]=%hu\n",
	      i, prng.branch[i]);
    }
  }
  for (i = 0; i < 3; ++i) {
    prng.xsubi[i] = prng.branch[i];
  }
  prngmunge();
}

static void prngseed_smart(void)
{
  FILE *rfp;
  int rv;

  rfp = fopen("/dev/urandom", "r");
  rv = 0;
  if (rfp) {
    rv = fread(&prng, 1, sizeof(prng), rfp);
    fclose(rfp);
  }
  if (gparm.verbose && (gparm.verbose > 1 || rv != sizeof(prng))) {
    fprintf(stderr, "Got %d out of expected %d bytes from /dev/urandom\n",
	    rv, (int)sizeof(prng));
  }
}

struct protinfo {
  /* information about a supported protocol */
  char *name; /* name for use in command line */
  void *usr; /* arbitrary argument for the callback functions */
  struct protinst *(*initproc)(struct protinfo *pi, int argc, char **argv, int *argi); /* create context; parse arguments (using getopt()); any other setup */
  int defport; /* default TCP port number */
};

struct protinst {
  /* information about an initialized protocol */
  void *usr; /* arbitrary argument for the callback functions */
  struct conninfo *(*connproc)(struct protinst *pi, int sok); /* initialize a connection; the sok parameter is a new connected socket */
};

enum connstatus {
  cs_ok, /* all went well */
  cs_fatal, /* error which kills the connection */
  cs_transient, /* error which might be ok next time */
  cs_close /* normal close is occurring */
};

struct conninfo {
  /* information about a connection */
  void *usr; /* arbitrary argument for the callback functions */
  char *label; /* a label for this connection */
  int sok; /* file descriptor number for socket we use */
  void (*closeproc)(struct conninfo *ci); /* when connection is closed, or transferred to another process */
  /* At any given time, the connection may have a read proc, a write proc,
   * and a timer, or any combination of those.
   */
  enum connstatus (*readproc)(struct conninfo *ci);
  enum connstatus (*writeproc)(struct conninfo *ci);
  long long timer; /* microsecond time to run timerproc() if there is one */
  enum connstatus (*timerproc)(struct conninfo *ci);

  struct conninfo *next; /* so we can link these into a list */
};

struct onebuf {
  char buf[512];
  int num, used;
  /* the "interesting" part of the buffer is buf[used...(num-1)] */
};

struct onetime {
  char *buf;
  int len, wrote;
};

/* simple_close(): a closeproc for simple cases where what needs to be
 * done is call free() on ci->usr (the caller will free 'ci').
 */
static void simple_close(struct conninfo *ci)
{
  if (ci->usr) { free(ci->usr); }
}

/* simple_init(): simple initializer that just sets the conn proc as specified
 * in usr, and sets usr to NULL, and takes no arguments.
 */
static struct protinst *simple_init(struct protinfo *pi, int argc, char **argv,
				    int *argi)
{
  struct protinst *pinst;
  New(pinst);
  pinst->usr = NULL;
  pinst->connproc = pi->usr;
  return(pinst);
}

/* conn_read(): read data on a connection, into a buffer */
static enum connstatus conn_read(struct conninfo *ci,
				 char *buf, int bufsz, int *got)
{
  int rv;
  if (got) { *got = 0; }
  if (gparm.verbose > 1) {
    fprintf(stderr, "read(%d, %p, %d)\n", ci->sok, buf, (int)bufsz);
  }
  rv = read(ci->sok, buf, bufsz);
  if (rv < 0) {
    if (errno == EAGAIN || errno == EINTR) {
      return(cs_transient);
    } else if (errno == ECONNRESET || errno == EPIPE) {
      return(cs_close);
    } else {
      fprintf(stderr, "read%s: %s\n",
	      ci->label ? ci->label : "", strerror(errno));
      return(cs_fatal);
    }
  }
  if (gparm.verbose > 1) {
    fprintf(stderr, "read() returned %d\n", (int)rv);
  }
  if (rv == 0) {
    return(cs_close);
  }
  if (rv > bufsz) {
    fprintf(stderr, "read%s: internal error, buffer size\n",
	    ci->label ? ci->label : "");
    return(cs_fatal);
  }
  if (got) { *got = rv; }
  return(cs_ok);
}

/* conn_write(): write data on a connection, from a buffer */
static enum connstatus conn_write(struct conninfo *ci, char *buf,
				  int len, int *wrote)
{
  int rv;
  if (wrote) { *wrote = 0; }
  if (gparm.verbose > 1) {
    fprintf(stderr, "write(%d, %p, %d)\n", ci->sok, buf, (int)len);
  }
  rv = write(ci->sok, buf, len);
  if (rv < 0) {
    if (errno == EAGAIN || errno == EINTR) {
      return(cs_transient);
    } else if (errno == ECONNRESET || errno == EPIPE) {
      return(cs_close);
    } else {
      fprintf(stderr, "write%s: %s\n",
	      ci->label ? ci->label : "", strerror(errno));
      return(cs_fatal);
    }
  }
  if (gparm.verbose > 1) {
    fprintf(stderr, "write() returned %d\n", (int)rv);
  }
  if (rv == 0) {
    return(cs_close);
  }
  if (rv > len) {
    fprintf(stderr, "write%s: internal error, buffer size\n",
	    ci->label ? ci->label : "");
    return(cs_fatal);
  }
  if (wrote) { *wrote = rv; }
  return(cs_ok);
}

static enum connstatus echo_write(struct conninfo *ci);

/* echo_read(): receive data in the ECHO protocol */
static enum connstatus echo_read(struct conninfo *ci)
{
  struct onebuf *ob = ci->usr;
  enum connstatus cs;

  if ((cs = conn_read(ci, ob->buf, sizeof(ob->buf), &(ob->num))) != cs_ok) {
    if (gparm.verbose > 1) {
      fprintf(stderr, "echo_read() got status %d\n", (int)cs);
    }
    return(cs);
  }
  ob->used = 0;
  if (ob->num > 0) {
    ci->readproc = NULL;
    ci->writeproc = &echo_write;
    if (gparm.verbose > 1) {
      fprintf(stderr, "echo_read(), conn '%s' has %d bytes, ready to write\n",
	      ci->label, (int)ob->num);
    }
  }
  return(cs_ok);
}

/* echo_write(): send data in the ECHO protocol */
static enum connstatus echo_write(struct conninfo *ci)
{
  struct onebuf *ob = ci->usr;
  int wrote;
  enum connstatus cs;

  if ((cs = conn_write(ci, ob->buf + ob->used, ob->num - ob->used,
		       &wrote)) != cs_ok) {
    return(cs);
  }
  ob->used += wrote;
  if (ob->used == ob->num) {
    ob->used = ob->num = 0;
    ci->readproc = &echo_read;
    ci->writeproc = NULL;
  }
  return(cs_ok);
}

/* echo_conn(): initialize a connection in the ECHO protocol */
static struct conninfo *echo_conn(struct protinst *pi, int sok)
{
  struct conninfo *ci;
  struct onebuf *ob;

  if (gparm.verbose > 1) {
    fprintf(stderr, "echo_conn()\n");
  }
  New(ci);
  New(ob);
  ci->usr = ob;
  ci->sok = sok;
  ci->label = NULL; /* will be filled in later */
  ob->num = ob->used = 0;
  ci->closeproc = &simple_close;
  ci->readproc = &echo_read;
  ci->writeproc = NULL; /* will be set when there's something to write */
  ci->timerproc = NULL; /* not used in this protocol */
  return(ci);
}

/* disc_read(): receive data in the discard protocol */
static enum connstatus disc_read(struct conninfo *ci)
{
  char buf[512];
  return(conn_read(ci, buf, sizeof(buf), NULL));
}

/* disc_conn(): initialize a connection in the Discard protocol */
static struct conninfo *disc_conn(struct protinst *pi, int sok)
{
  struct conninfo *ci;

  if (gparm.verbose > 1) {
    fprintf(stderr, "disc_conn()\n");
  }
  New(ci);
  ci->sok = sok;
  ci->usr = NULL;
  ci->label = NULL; /* will be filled in later */
  ci->closeproc = &simple_close;
  ci->readproc = &disc_read;
  ci->writeproc = NULL; /* not used in this protocol */
  ci->timerproc = NULL; /* not used in this protocol */
  return(ci);
}

/* onetime_close(): a closeproc for the case where usr is of type
 * 'struct onetime *'
 */
static void onetime_close(struct conninfo *ci)
{
  if (ci) {
    if (ci->usr) {
      struct onetime *ot = ci->usr;
      free(ot->buf);
      free(ot);
    }
  }
}

/* onetime_write(): write data from a buffer, then close the connection
 */
static enum connstatus onetime_write(struct conninfo *ci)
{
  struct onetime *ot = ci->usr;
  int wrote;
  enum connstatus cs;
  if ((cs = conn_write(ci, ot->buf + ot->wrote, ot->len - ot->wrote,
		       &wrote)) != cs_ok) {
    return(cs);
  }
  ot->wrote += wrote;
  if (ot->wrote == ot->len) {
    /* done writing! */
    ci->writeproc = NULL;
    return(cs_close);
  } else {
    return(cs_ok);
  }
}

/* daytime_conn(): initialize a connection in the Daytime protocol */
static struct conninfo *daytime_conn(struct protinst *pi, int sok)
{
  struct conninfo *ci;
  struct onetime *ot;
  int bufsz = 128;
  struct tm *tm;
  time_t t;

  if (gparm.verbose > 1) {
    fprintf(stderr, "daytime_conn()\n");
  }
  New(ci);
  New(ot);
  ci->sok = sok;
  ci->usr = ot;
  ot->buf = malloc(bufsz);
  if (!ot->buf) {
    perror("memory management failure");
    exit(2);
  }
  ot->wrote = 0;
  t = time(NULL);
  tm = localtime(&t);
  
  ot->len = strftime(ot->buf, bufsz, "%a %b %d %H:%M:%S %Y\r\n", tm);
  if (ot->len > bufsz) {
    fprintf(stderr, "Internal error formatting date\n");
    free(ci);
    free(ot);
    return(NULL);
  }

  ci->label = NULL; /* will be filled in later */
  ci->closeproc = &onetime_close;
  ci->readproc = &disc_read;
  ci->writeproc = &onetime_write;
  ci->timerproc = NULL; /* not used in this protocol */
  return(ci);
}

/* time_conn(): initialize a connection in the Time protocol */
static struct conninfo *time_conn(struct protinst *pi, int sok)
{
  struct conninfo *ci;
  struct onetime *ot;
  time_t t;
  unsigned tt;

  if (gparm.verbose > 1) {
    fprintf(stderr, "time_conn()\n");
  }
  New(ci);
  New(ot);
  ci->sok = sok;
  ci->usr = ot;
  ot->buf = malloc(4);
  if (!ot->buf) {
    perror("memory management failure");
    exit(2);
  }
  ot->wrote = 0;
  t = time(NULL);
  tt = t;
  tt += 2208988800LL; /* according to RFC 868 */
  ot->len = 4;
  /* RFC 868 doesn't say whether the number we send should be big endian
   * or little endian; but I'd assume it's big endian 
   */
  ot->buf[0] = tt >> 24;
  ot->buf[1] = tt >> 16;
  ot->buf[2] = tt >> 8;
  ot->buf[3] = tt;

  ci->label = NULL; /* will be filled in later */
  ci->closeproc = &onetime_close;
  ci->readproc = &disc_read;
  ci->writeproc = &onetime_write;
  ci->timerproc = NULL; /* not used in this protocol */
  return(ci);
}

struct qotd {
  /* info for the qotd service */
  int mnw, mxw; /* min & max numbers of words in output */
  int dnw; /* number of words in dictionary */
  int mwl; /* maximum word length */
  char **dict; /* the dictionary */
};

/* qotd_conn(): initialize a connection in the Quote of the Day protocol */
static struct conninfo *qotd_conn(struct protinst *pi, int sok)
{
  struct conninfo *ci;
  struct onetime *ot;
  int buflen, nw;
  struct qotd *q = pi->usr;
  int i, w, cap;

  if (gparm.verbose > 1) {
    fprintf(stderr, "qotd_conn()\n");
  }
  New(ci);
  New(ci);
  New(ot);
  ci->sok = sok;
  ci->usr = ot;
  ot->buf = malloc(4);
  if (!ot->buf) {
    perror("memory management failure");
    exit(2);
  }
  ot->wrote = 0;

  /* Fake a quote */
  cap = 1; /* capitalization */
  if (q->mxw > q->mnw) {
    nw = q->mnw + (nrand48(prng.xsubi) % (q->mxw - q->mnw + 1));
  } else {
    nw = q->mnw;
  }
  buflen = (q->mwl + 4) * nw + 10;
  if (!(ot->buf = malloc(buflen))) {
    perror("memory management failure");
    exit(2);
  }
  ot->len = 0;
  for (i = 0; i < nw; ++i) {
    if (i) {
      switch(nrand48(prng.xsubi) % 36) {
      case 0:
      case 1:
      case 2: strcpy(ot->buf + ot->len, ", "); ot->len += 2; break;
      case 3:
      case 4: strcpy(ot->buf + ot->len, ".  "); ot->len += 3; cap = 1; break;
      case 5: strcpy(ot->buf + ot->len, " -- "); ot->len += 4; break;
      default: ot->buf[ot->len++] = ' '; break;
      }
    }
    w = nrand48(prng.xsubi) % q->dnw;
    if (cap) {
      ot->buf[ot->len] = toupper(q->dict[w][0]);
    } else {
      ot->buf[ot->len] = q->dict[w][0];
    }
    cap = 0;
    strcpy(ot->buf + ot->len + 1, q->dict[w] + 1);
    ot->len += strlen(q->dict[w]);
  }
  switch (nrand48(prng.xsubi) % 10) {
  case 0:
  case 1: break;
  case 2: ot->buf[ot->len++] = '!'; break;
  default: ot->buf[ot->len++] = '.'; break;
  }
  ot->buf[ot->len++] = '\r';
  ot->buf[ot->len++] = '\n';
  ot->buf[ot->len++] = '\0';
  
  ci->label = NULL; /* will be filled in later */
  ci->closeproc = &onetime_close;
  ci->readproc = &disc_read;
  ci->writeproc = &onetime_write;
  ci->timerproc = NULL; /* not used in this protocol */
  return(ci);
}

/* qotd_init(): initialize a "qotd" service, parsing the command line options
 * for it
 */
static struct protinst *qotd_init(struct protinfo *pi, int argc, char **argv,
				    int *argi)
{
  struct protinst *pinst;
  struct qotd *q;
  char *df = NULL;
  FILE *dfp;
  char wb[64];
  int dall, len, i;

  New(pinst);
  New(q);
  pinst->usr = q;
  pinst->connproc = &qotd_conn;

  q->mnw = q->mxw = 5;
  for (;;) {
    if ((1+*argi) < argc && !strcmp(argv[*argi], "-d")) {
      df = argv[1+*argi];
      *argi += 2;
    } else if ((1+*argi) < argc && !strcmp(argv[*argi], "-w")) {
      int f = sscanf(argv[1+*argi], "%d-%d", &q->mnw, &q->mxw);
      if (f == 1) {
	q->mxw = q->mnw;
      }
      if (f < 1 || (f > 1 && q->mxw <= q->mnw) ||
	  q->mxw < 1) {
	fprintf(stderr, "Bad -w argument to 'qotd': %s\n", argv[1+*argi]);
	usage();
      }
      *argi += 2;
    } else {
      /* there must not be any more parameters for "qotd" */
      break;
    }
  }
  if (df) {
    dfp = fopen(df, "r");
  } else {
    dfp = fopen("/usr/dict/words", "r");
    if (!dfp) {
      dfp = fopen("/usr/share/dict/words", "r");
    }
  }
  
  /* now read the dictionary */
  q->dnw = 0;
  dall = 0;
  q->dict = NULL;

  while (dfp && !feof(dfp) && !ferror(dfp)) {
    wb[0] = '\0';
    fgets(wb, sizeof(wb), dfp);
    len = strlen(wb);
    if (len > 0 && wb[len - 1] == '\n') {
      wb[--len] = '\0';
    }
    /* filter the dictionary word */
    if (len < 3 || len > 8) { continue; }
    for (i = 0; i < len; ++i) {
      if (!isalpha(wb[i]) || !islower(wb[i])) {
	break;
      }
    }
    if (i < len) {
      /* we saw a character that didn't belong; skip this word */
      continue;
    }
    /* store the dictionary word */
    if (q->dnw >= dall) {
      dall += 1 + (dall >> 2); /* allocate 25% more memory */
      q->dict = realloc(q->dict, sizeof(q->dict[0]) * dall);
      if (!q->dict) {
	perror("memory management failure");
	exit(2);
      }
    }
    if (!(q->dict[q->dnw] = strdup(wb))) {
      perror("memory management failure");
      exit(2);
    }
    q->dnw++;
  }
  if ((dfp && ferror(dfp)) || q->dnw < 3) {
    static char *built_in_dict[] = {
      "it", "is", "annoying", "that", "your", "dictionary", "is", "missing",
      NULL
    };
    fprintf(stderr, "Problem with dictionary file; using built-in one\n");
    q->dict = built_in_dict;
    for (q->dnw = 0; built_in_dict[q->dnw]; ++q->dnw)
      ;
  }
  if (dfp) {
    fclose(dfp);
  }
  q->mwl = 0;
  for (i = 0; i < q->dnw; ++i) {
    len = strlen(q->dict[i]);
    if (q->mwl < len) {
      q->mwl = len;
    }
  }

  return(pinst);
}

/* chargen_write(): write characters in the Character Generator protocol;
 * in particular, write what's described as "one popular pattern" in RFC 864.
 * The variable 'state' pointed to by 'ci->usr' gives the number of characters
 * we've written so far, including returns and line feeds.  (Modulo 7030
 * which is the length of the pattern.)
 */
static enum connstatus chargen_write(struct conninfo *ci)
{
  char buf[512];
  int *state = ci->usr;
  int i, es, pil, ln, cn, wrote;
  enum connstatus cs;

  for (i = 0; i < sizeof(buf); ++i) {
    /* figure out 'effective state' */
    es = *state + i;
    /* figure out the character here */
    pil = es % 74;
    if (pil >= 72) {
      /* CRLF at end of each line */
      if (pil == 72) {
	buf[i] = '\r';
      } else {
	buf[i] = '\n';
      }
    } else {
      /* it's somewhere in the line */
      ln = es / 74;
      cn = (ln + pil) % 95;
      /* use 'cn' to choose among the printing ASCII characters including space */
      buf[i] = 32 + cn;
    }
  }

  if ((cs = conn_write(ci, buf, sizeof(buf), &wrote)) != cs_ok) {
    return(cs);
  }
  *state = (*state + wrote) % 7030;
  return(cs_ok);
}

/* chargen_conn(): initialize a connection in the Character Generator protocol */
static struct conninfo *chargen_conn(struct protinst *pi, int sok)
{
  struct conninfo *ci;
  int *state;

  New(ci);
  New(state);
  ci->sok = sok;
  ci->usr = state;
  ci->label = NULL; /* will be filled in later */
  ci->closeproc = &simple_close;
  ci->readproc = &disc_read;
  ci->writeproc = &chargen_write;
  ci->timerproc = NULL; /* not used in this protocol */
  return(ci);
}

struct gen_info {
  /* configuration and state for the "gen" protocol or any of its connections */
  /* config */
  long long interval_usec; /* microseconds between messages */
  long long random_usec; /* additional random microseconds */
  long long nmsg; /* number of messages before termination */
  long long delay_usec; /* delay before termination */
  /* state */
  long long msg_ctr; /* messages sent so far */
  /* data to be sent */
  char buf[128];
  int write, wrote; /* bytes written & left to write, in buf */
  char hn[128]; /* result of gethostname() if it succeeded */
};

/* gen_end_timer(): timer callback for "gen" protocol (during termination,
 * not normal operation)
 */
static enum connstatus gen_end_timer(struct conninfo *ci)
{
  return(cs_close);
}

static enum connstatus gen_timer(struct conninfo *ci);

/* gen_write(): Write some of the message for the "gen" service */
static enum connstatus gen_write(struct conninfo *ci)
{
  struct gen_info *gi = ci->usr;
  enum connstatus cs;
  int wrote;

  if (gi->write > 0) {
    if ((cs = conn_write(ci, gi->buf + gi->wrote, gi->write, &wrote)) != cs_ok) {
      return(cs);
    }
    gi->wrote += wrote;
    gi->write -= wrote;
    if (gi->write <= 0) {
      /* done with a message */
      gi->msg_ctr++;
      if (gi->nmsg < 1 || gi->msg_ctr < gi->nmsg) {
	/* set timer for next message */
	ci->timerproc = &gen_timer;
	ci->timer = usnow + gi->interval_usec;
      } else {
	/* no next message */
	if (gi->delay_usec < 1) {
	  return(cs_close);
	} else {
	  ci->timerproc = &gen_end_timer;
	  ci->timer = usnow + gi->delay_usec;
	}
      }
      ci->writeproc = NULL;
    }
  }
  return(cs_ok);
}

/* gen_timer(): timer callback for "gen" protocol (during normal operation,
 * not termination)
 */
static enum connstatus gen_timer(struct conninfo *ci)
{
  struct gen_info *gi = ci->usr;
  struct tm *tm;
  struct timeval t;
  char tbuf[64];

  memset(&t, 0, sizeof(t));
  gettimeofday(&t, NULL);
  tm = localtime(&(t.tv_sec));
  memset(tbuf, 0, sizeof(tbuf));
  strftime(tbuf, sizeof(tbuf), "%F %H:%M:%S", tm);

  gi->wrote = 0;
  gi->write = snprintf(gi->buf, sizeof(gi->buf),
		       "%s.%06u - msg %lld, pid %d, fd %d%s%s\r\n",
		       tbuf, (unsigned)t.tv_usec,
		       (long long)gi->msg_ctr, (int)getpid(),
		       (int)ci->sok,
		       gi->hn[0] ? ", host " : "",
		       gi->hn);
  ci->writeproc = &gen_write;
  ci->timerproc = NULL; /* don't restart timer until done writing */

  return(cs_ok);
}

/* gen_conn(): initialize connection for "gen" protocol */
static struct conninfo *gen_conn(struct protinst *pi, int sok)
{
  struct conninfo *ci;
  struct gen_info *gi = pi->usr, *gi2;

  New(ci);
  New(gi2);
  ci->sok = sok;
  *gi2 = *gi;
  gi2->msg_ctr = 0;
  gi2->write = gi2->wrote = 0;
  ci->usr = gi2;
  if ((gethostname(gi2->hn, sizeof(gi2->hn))) >= 0) {
    gi2->hn[sizeof(gi2->hn)-1] = '\0';
  } else {
    gi2->hn[0] = '\0';
  }

  ci->label = NULL; /* will be filled in later */

  ci->closeproc = &simple_close;
  ci->readproc = &disc_read;
  ci->writeproc = NULL;
  ci->timer = usnow;
  ci->timerproc = &gen_timer;
  return(ci);
}

static long long parse_interval_us(char *s)
{
  char *e = NULL;
  double x;
  long long y;

  x = strtod(s, &e);
  if ((e && *e) || x < 0) {
    fprintf(stderr, "Invalid time interval %s\n", s);
    exit(1);
  }
  y = x * 1000000 + 0.5;
  return(y);
}

/* gen_init(): initialize the "gen" protocol; this protocol is not
 * part of any standard but I find it convenient sometimes
 */
static struct protinst *gen_init(struct protinfo *pi, int argc, char **argv,
				 int *argi)
{
  struct protinst *pinst;
  struct gen_info *gi;
  char *e;

  New(pinst);
  New(gi);

  pinst->usr = gi;
  gi->interval_usec = 1000000;
  gi->random_usec = 0;
  gi->nmsg = 0;
  gi->delay_usec = 0;
  for (;;) {
    if ((1+*argi) < argc && !strcmp(argv[*argi], "-i")) {
      gi->interval_usec = parse_interval_us(argv[1+*argi]);
      *argi += 2;
    } else if ((1+*argi) < argc && !strcmp(argv[*argi], "-r")) {
      gi->random_usec = parse_interval_us(argv[1+*argi]);
      *argi += 2;
    } else if ((1+*argi) < argc && !strcmp(argv[*argi], "-n")) {
      e = NULL;
      gi->nmsg = strtoll(argv[1+*argi], &e, 0);
      if (gi->nmsg < 0 || (e && *e)) { gi->nmsg = 0; }
      *argi += 2;
    } else if ((1+*argi) < argc && !strcmp(argv[*argi], "-d")) {
      gi->delay_usec = parse_interval_us(argv[1+*argi]);
      *argi += 2;
    } else {
      /* there must be no more options for "gen" */
      break;
    }
  }

  pinst->connproc = &gen_conn;
  return(pinst);
}

static struct protinfo protos[] = {
  { "echo", &echo_conn, &simple_init, 7 },
  { "discard", &disc_conn, &simple_init, 9 },
  { "daytime", &daytime_conn, &simple_init, 13 },
  { "time", &time_conn, &simple_init, 37 },
  { "chargen", &chargen_conn, &simple_init, 19 },
  { "qotd", NULL, &qotd_init, 17 },
  { "gen", NULL, &gen_init, -1 },

  { NULL, NULL, NULL, -1 }
};

struct listen1 {
  /* one address we listen on */
  struct listen1 *next; /* there can be more than one */
  char *aspec; /* string that was used to specify the address, if any */
  int lsok; /* file descriptor of socket we listen on */
  struct sockaddr *addr; /* the address of it */
  socklen_t alen; /* length of that address */
};

static void handle_sigchld(int i)
{
  /* This function does nothing.  It's just there so that we're not, technically,
   * ignoring SIGCHLD and it hopefully will interrupt any signal() we've got
   * going on.
   */
}

static void handle_sigusr1(int i)
{
  /* SIGUSR1: results in the 'verbose' value cycling through 0, 1, 2 */
  gparm.verbose = (gparm.verbose + 1) % 3;
}

static void handle_sigusr2(int i)
{
  /* SIGUSR2: results in some information being dumped to stderr */
  gparm.sigusr2_pending = 1;
}

/* main(): As always, the "main body" of the program.  Calls whatever other
 * functions are needed to make things happen.
 */
int main(int argc, char *argv[])
{
  char pbuf[16], lbuf[512];
  char hbuf[256], sbuf[64];
  char *pname, *host, *port, *hostport, *e;
  int oc, i, rv, af, boff, closit, max_fd, nconns = 0;
  int selnr, selnw, selnc, we_are_child = 0;
  long long togo, least_togo;
  socklen_t alen;
  enum connstatus cs;
  fd_set rfds, wfds;
  struct timeval tv;
  struct sigaction siga;
  struct addrinfo aihints, *aires;
#ifdef DO_IPv6
  struct sockaddr_in6 *a6, ab6;
#endif
  struct sockaddr_in *a, ab;
  struct sockaddr *sap;
  struct protinfo *proto;
  struct protinst *pinst;
  struct conninfo *conns = NULL, *ct, **ctp, **ctp2;
  struct listen1 *listens = NULL, *lt;

  /* *** *** Defaults *** *** */

  gparm.verbose = 0;
  gparm.verbose_extra = 0;
  gparm.conns_per_proc = 100;
#ifdef DO_IPv6
  gparm.ipv6 = 0;
#endif
  gparm.numeric = 0;
  gparm.sigusr2_pending = 0;

  /* *** *** Parse the command line *** *** */
  /* Parse global options */
  for (;;) {
    oc = getopt(argc, argv, "N:vV:n"
#ifdef DO_IPv6
		"6"
#endif
		);
    if (oc < 0) {
      /* end of the global options */
      break;
    }
    switch (oc) {
    case 'N':
      e = NULL;
      if ((gparm.conns_per_proc = strtol(optarg, &e, 0)) < 0 || (e && *e)) {
	fprintf(stderr, "option -n must be a number at least 0\n");
	usage();
      }
      break;
    case 'v': gparm.verbose++; break;
    case 'V':
      for (i = 0; optarg[i]; ++i) {
	/* individual characters in optarg, identify specific
	 * messages.
	 *	'b' - in backoff_delay()
	 *	'u' - in update_usnow()
	 */
	gparm.verbose_extra ^= VERBOSE_EXTRA_BIT(optarg[i]);
      }
#ifdef DO_IPv6
    case '6': gparm.ipv6 = 1; break;
#endif
    case 'n': gparm.numeric = 1; break;
    default: usage();
    }
  }

  /* parse the protocol name */
  if (optind >= argc) {
    usage();
  }
  pname = argv[optind++];
  for (proto = protos; proto->name && strcasecmp(proto->name, pname); proto++)
    ;
  if (!(proto && proto->name)) {
    fprintf(stderr,
	    "Unknown protocol name '%s'.\n"
	    "Recognized values:\n", pname);
    for(proto = protos; proto->name; ++proto) {
      fprintf(stderr, "\t%s\n", proto->name);
    }
    exit(1);
  }

  /* initialize pseudo random number generation */

  prngseed_dumb();
  prngseed_smart();

  /* initialize the protocol; this will parse any protocol specific options */
  
  i = optind;
  pinst = (*proto->initproc)(proto, argc, argv, &i);
  optind = i;
  if (pinst == NULL) {
    fprintf(stderr, "Error initializing protocol '%s'\n", pname);
    exit(1);
  }

  if (gparm.verbose) {
    fprintf(stderr, "Global parameters: verbose=%d verbose_extra=0x%llx"
	    " conns_per_proc=%d"
#ifdef DO_IPv6
	    " ipv6=%d"
#endif
	    " numeric=%d.\n",
	    (int)gparm.verbose, (unsigned long long)gparm.verbose_extra,
	    (int)gparm.conns_per_proc,
#ifdef DO_IPv6
	    (int)gparm.ipv6,
#endif
	    (int)gparm.numeric);
  }

  /* parse the addresses if any */
  if (optind < argc) {
    while (optind < argc) {
      /* split up the address/port */
      hostport = strdup(argv[optind]);
      if (!hostport) { perror("memory management failure"); exit(2); }
      port = strrchr(hostport, '/');
      if (port) {
	if (port == hostport) {
	  host = NULL;
	} else {
	  host = hostport;
	}
	*port = '\0';
	++port;
      } else {
	host = hostport;
	if (proto->defport >= 0) {
	  snprintf(pbuf, sizeof(pbuf), "%d", proto->defport);
	  port = pbuf;
	} else {
	  fprintf(stderr, "Protocol '%s' needs port specified, has no default\n",
		  proto->name);
	  exit(1);
	}
      }
    
      /* look them up */
      memset(&aihints, 0, sizeof(aihints));
      aihints.ai_family =
#ifdef DO_IPv6
	gparm.ipv6 ? PF_INET6 :
#endif
	PF_INET;
      aihints.ai_socktype = SOCK_STREAM;
      aihints.ai_protocol = IPPROTO_TCP;
      aihints.ai_flags = AI_ADDRCONFIG | AI_PASSIVE |
	(gparm.numeric ? (AI_NUMERICHOST
#ifdef HAS_AI_NUMERICSERV
			  | AI_NUMERICSERV
#endif
			  ) : 0);
      aires = NULL;
      if (gparm.verbose) {
	fprintf(stderr, "Looking up address: host '%s' port '%s' af %d\n",
		host, port, (int)aihints.ai_family);
      }
      rv = getaddrinfo(host, port, &aihints, &aires);
      if (rv) {
	fprintf(stderr, "Error interpreting address '%s': %s\n",
		argv[optind], gai_strerror(rv));
	exit(1);
      }
      
      /* take the first match, if any; if there wasn't, that's an error */
      if (!aires) {
	fprintf(stderr, "Address '%s' not found\n", argv[optind]);
	exit(1);
      }
      
      New(lt);
      lt->next = listens;
      lt->aspec = strdup(argv[optind]);
      lt->lsok = -1; /* will be set later */
      lt->alen = aires->ai_addrlen;
      if ((lt->addr = malloc(lt->alen)) == NULL) {
	perror("memory management failure");
	exit(1);
      }
      memcpy(lt->addr, aires->ai_addr, lt->alen);
      listens = lt;
    
      free(hostport);
      ++optind;
      freeaddrinfo(aires);
      aires = NULL;
    }
  } else {
    /* no address listed; use default */
    int p;

    if (proto->defport >= 0) {
      p = proto->defport;
    } else {
      fprintf(stderr, "Protocol '%s' needs port specified, has no default\n",
	      proto->name);
      exit(1);
    }

    if (gparm.verbose) {
      fprintf(stderr, "Will listen on default address, port %d\n", p);
    }

    New(listens);
    listens->next = NULL;
    listens->aspec = "(default)";
    listens->lsok = -1;
#ifdef DO_IPv6
    if (gparm.ipv6) {
      New(a6);
      listens->addr = (void *)a6;
      listens->alen = sizeof(*a6);
#if 0
      a6->sin6_len = sizeof(*a6); /* XXX doesn't exist on Linux */
#endif
      a6->sin6_family = AF_INET6;
      a6->sin6_port = htons(p);
      /* a6->sin6_addr -- New() did a memset() */
    } else {
#endif /* DO_IPv6 */
      New(a);
      listens->addr = (void *)a;
      listens->alen = sizeof(*a);
      a->sin_family = AF_INET;
      a->sin_port = htons(p);
      a->sin_addr.s_addr = INADDR_ANY;
#ifdef DO_IPv6
    }
#endif
  }

  /* *** *** set up sockets to listen on the specified addresses *** *** */
  max_fd = 0;
  for (lt = listens; lt; lt = lt->next) {
    af = AF_INET;
#ifdef DO_IPv6
    if (gparm.ipv6) {
      af = AF_INET6;
    }
#endif /* DO_IPv6 */
    if ((lt->lsok = socket(af, SOCK_STREAM, IPPROTO_TCP)) < 0) {
      fprintf(stderr, "Error trying to listen on '%s': socket(): %s\n",
	      lt->aspec, strerror(errno));
      exit(2);
    }
    if (max_fd < lt->lsok) {
      max_fd = lt->lsok;
    }
    if (gparm.verbose) {
      hbuf[0] = sbuf[0] = '\0';
      rv = getnameinfo(lt->addr, lt->alen,
		       hbuf, sizeof(hbuf),
		       sbuf, sizeof(sbuf), NI_NUMERICHOST|NI_NUMERICSERV);
      fprintf(stderr,
	      "On socket %d, going to listen for connections to: %s/%s (%s)\n",
	      (int)lt->lsok, (rv||!hbuf[0]) ? "?" : hbuf,
	      (rv || !sbuf[0]) ? "?" : sbuf, lt->aspec);
    }
    if (bind(lt->lsok, lt->addr, lt->alen) < 0) {
      fprintf(stderr, "Error trying to listen on '%s': bind(): %s\n",
	      lt->aspec, strerror(errno));
      exit(2);
    }
    if (listen(lt->lsok, 25) < 0) {
      fprintf(stderr, "Error trying to listen on '%s': listen(): %s\n",
	      lt->aspec, strerror(errno));
      exit(2);
    }
    if (gparm.verbose) {
      fprintf(stderr, "Set up listening socket on '%s': fd %d\n",
	      lt->aspec, (int)lt->lsok);
    }
  }

  /* *** *** main loop: listen for connections and serve them *** *** */
  
  signal(SIGPIPE, SIG_IGN);
  siga.sa_flags = SA_NOCLDSTOP;
  sigemptyset(&siga.sa_mask);
  siga.sa_handler = &handle_sigchld;
  sigaction(SIGCHLD, &siga, NULL);
  siga.sa_flags = 0;
  sigemptyset(&siga.sa_mask);
  siga.sa_handler = &handle_sigusr1;
  sigaction(SIGUSR1, &siga, NULL);
  siga.sa_flags = 0;
  sigemptyset(&siga.sa_mask);
  siga.sa_handler = &handle_sigusr2;
  sigaction(SIGUSR2, &siga, NULL);
  for (;;) {
    if (gparm.sigusr2_pending) {
      gparm.sigusr2_pending = 0;
      fprintf(stderr, "SIGUSR2 INFO DUMP:\n");
      fprintf(stderr, "\tListening ports:\n");
      for (lt = listens; lt; lt = lt->next) {
	fprintf(stderr, "\t\tstruct %p spec '%s' lsok %d\n",
		lt, lt->aspec, (int)lt->lsok);
      }
      fprintf(stderr, "\tNumber of connections: %d\n", (int)nconns);
      fprintf(stderr, "\tConnections:\n");
      for (ct = conns; ct; ct = ct->next) {
	fprintf(stderr, "\t\tstruct %p usr %p label '%s' sok %d",
		ct, ct->usr, ct->label, (int)ct->sok);
	if (ct->closeproc) {
	  fprintf(stderr, " close=%p", ct->closeproc);
	}
	if (ct->readproc) {
	  fprintf(stderr, " read=%p", ct->readproc);
	}
	if (ct->writeproc) {
	  fprintf(stderr, " write=%p", ct->writeproc);
	}
	if (ct->timerproc) {
	  fprintf(stderr, " timer=%p (%lld us from now)",
		  ct->timerproc, (long long)(ct->timer - usnow));
	}
	fputc('\n', stderr);
      }
    }

    /* reap any child processes */
#ifdef WAITPID_MINUS_ONE
    if (gparm.conns_per_proc > 1) {
      while ((rv = waitpid(-1, &i, WNOHANG)) > 0) {
	if (gparm.verbose) {
	  if (WIFEXITED(i)) {
	    fprintf(stderr, "Child process %d exited with status %d%s\n",
		    (int)rv, (int)WEXITSTATUS(i),
		    WEXITSTATUS(i) ? "" : " (normal)");
	  } else if (WIFSIGNALED(i)) {
	    fprintf(stderr, "Child process %d terminated with signal %d%s\n",
		    (int)rv, (int)WTERMSIG(i),
		    WCOREDUMP(i) ? " (core dumped)" : "");
	  }
	}
      }
      if (rv < 0) {
	if (errno == ECHILD) {
	  /* no problem */
	} else {
	  perror("waitpid");
	  backoff_delay(1);
	}
      }
    }
#endif /* WAITPID_MINUS_ONE */

    /* figure out what time it is now */
    update_usnow();
    
    /* Go through the sockets we listen on, and the connections we've got open,
     * and any timers on them, and prepare them all for select().
     */
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    least_togo = 20000000;

    selnr = selnw = selnc = 0;

    for (lt = listens; lt && !we_are_child; lt = lt->next) {
      /* see if a connection is coming in on this socket */
      FD_SET(lt->lsok, &rfds);
      ++selnc;
    }
    for (ct = conns; ct; ct = ct->next) {
      if (gparm.verbose > 1) {
	fprintf(stderr, "Select preparation, %p '%s' fd=%d\n",
		ct, ct->label, ct->sok);
      }
      if (ct->readproc) { FD_SET(ct->sok, &rfds); ++selnr; }
      if (ct->writeproc) { FD_SET(ct->sok, &wfds); ++selnw; }
      if (ct->timerproc) {
	togo = ct->timer - usnow;
	if (togo < 0) { togo = 0; }
	if (least_togo > togo) { least_togo = togo; }
      }
    }

    if (gparm.verbose) {
      fprintf(stderr,
	      "About to select(), time %lld usec, %d read, %d write, %d listen, nfds %d\n",
	      (long long)least_togo, (int)selnr, (int)selnw, (int)selnc,
	      (int)(max_fd + 1));
    }

    /* Now, use select() to wait until something happens or a timer runs out */
    tv.tv_sec = least_togo / 1000000;
    tv.tv_usec = least_togo % 1000000;
    rv = select(max_fd + 1, &rfds, &wfds, NULL, &tv);
    if (rv < 0) {
      if (errno == EAGAIN || errno == EINTR) {
	/* not really errors */
	backoff_delay(0);
      } else {
	perror("system error while waiting using select()");
	backoff_delay(1);
      }
      continue;
    }

    /* and see what select() has given us */

    boff = 0;
    update_usnow();

    for (ctp = &conns; *ctp; ctp = ctp2) {
      ct = *ctp;
      ctp2 = &(ct->next);
      closit = 0;
      if (ct->timerproc && ct->timer <= usnow) {
	/* This one. */
	if (gparm.verbose) {
	  fprintf(stderr, "Timer activated on connection '%s'\n", ct->label);
	}
	cs = ct->timerproc(ct);
	switch(cs) {
	case cs_ok: /* all was ok */ break;
	case cs_fatal: /* close due to error */ closit = 1; break;
	case cs_close: /* close now */ closit = 1; break;
	case cs_transient: /* error, try again */ boff++; break;
	}
      }
      if (!closit && ct->writeproc && FD_ISSET(ct->sok, &wfds)) {
	/* This connection can be written to */
	if (gparm.verbose) {
	  fprintf(stderr, "Write possible on connection '%s'\n", ct->label);
	}
	cs = ct->writeproc(ct);
	switch (cs) {
	case cs_ok: /* all was ok */ break;
	case cs_fatal: /* close due to error */ closit = 1; break;
	case cs_close: /* close now */ closit = 1; break;
	case cs_transient: /* error, try again */ boff++; break;
	}
      }
      if (!closit && ct->readproc && FD_ISSET(ct->sok, &rfds)) {
	/* This connection can be read from */
	if (gparm.verbose) {
	  fprintf(stderr, "Read possible on connection '%s'\n", ct->label);
	}
	cs = ct->readproc(ct);
	switch (cs) {
	case cs_ok: /* all was ok */ break;
	case cs_fatal: /* close due to error */ closit = 1; break;
	case cs_close: /* close now */ closit = 1; break;
	case cs_transient: /* error, try again */ boff++; break;
	}
      }
      if (closit) {
	/* close the connection and remove it from the list */
	if (gparm.verbose) {
	  fprintf(stderr, "Closing connection '%s'\n", ct->label);
	}
	*ctp = ct->next;
	ctp2 = ctp;
	if (ct->closeproc) {
	  ct->closeproc(ct);
	}
	if (ct->label) { free(ct->label); }
	close(ct->sok);
	free(ct);
	--nconns;
	if (we_are_child && nconns < 1) {
	  exit(0);
	}
      }
    }

    for (lt = listens; lt && !we_are_child; lt = lt->next) {
      if (FD_ISSET(lt->lsok, &rfds)) {
	/* a connection is coming in on this socket */
	sap = (void *)&ab;
	alen = sizeof(ab);
#ifdef DO_IPv6
	if (gparm.ipv6) {
	  sap = (void *)&ab6;
	  alen = sizeof(ab6);
	}
#endif /* DO_IPv6 */
	memset(sap, 0, alen);
	rv = accept(lt->lsok, sap, &alen);
	if (rv < 0) {
	  if (errno == EINTR) {
	    /* an interrupt came in while we were accepting a connection;
	     * act like nothing happened
	     */
	    continue;
	  } else if (errno == ECONNABORTED) {
	    /* connection has been aborted; that's an exceptional circumstance
	     * but not really an error
	     */
	    ++boff;
	    continue;
	  } else {
	    /* error! */
	    fprintf(stderr, "Error accepting connection on %s: %s\n",
		    lt->aspec, strerror(errno));
	    ++boff;
	    continue;
	  }
	}
	if (max_fd < rv) {
	  max_fd = rv;
	}
	if (gparm.verbose > 1) {
	  fprintf(stderr, "On accept(), got address:\n");
	  for (i = 0; i < alen; ++i) {
	    fprintf(stderr, "%s%02x%s",
		    (!(i & 7)) ? "\t" : " ",
		    (int)(((unsigned char *)sap)[i]),
		    ((i & 7) == 7 || (i == alen - 1)) ? "\n" : "");
	  }
	}

	/* So, we have a connection; record it */
	ct = pinst->connproc(pinst, rv);
	if (!ct) {
	  fprintf(stderr, "Error setting up connection on %s\n", lt->aspec);
	  close(rv);
	  ++boff;
	}
	ct->next = conns;
	hbuf[0] = sbuf[0] = '\0';

	rv = getnameinfo(sap, alen, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf),
			 NI_NUMERICHOST|NI_NUMERICSERV);
	snprintf(lbuf, sizeof(lbuf),
		 "(%s/%s->%s)",
		 (rv || !hbuf[0]) ? "?" : hbuf,
		 (rv || !sbuf[0]) ? "?" : sbuf, lt->aspec);
	ct->label = strdup(lbuf);
	conns = ct;
	++nconns;

	if (gparm.verbose) {
	  fprintf(stderr, "Connection '%s' received on '%s' (fd=%d)\n",
		  ct->label, lt->aspec, (int)ct->sok);
	}

      }
    }

    if (boff) {
      backoff_delay(0);
    }

    if ((!we_are_child) &&
	gparm.conns_per_proc > 0 &&
	nconns >= gparm.conns_per_proc) {
      /* So, we've got a bunch of connections; fork a new process
       * to handle them.
       */
      rv = fork();
      if (rv < 0) {
	if (errno == EAGAIN) {
	  ++boff;
	}
	if (gparm.verbose > 0) {
	  fprintf(stderr, "fork() failed: %s\n", strerror(errno));
	}
      } else if (rv == 0) {
	/* child process */
	we_are_child = 1;
	for (lt = listens; lt; lt = lt->next) {
	  /* only the parent listens */
	  close(lt->lsok);
	  lt->lsok = -1;
	}
	for (i = 0; i < 3; ++i) { prng.xsubi[i] = prng.branch[i]; }
      } else {
	/* parent process */
	if (gparm.verbose) {
	  fprintf(stderr, "Migrating %d connections to child process, pid %d\n",
		  (int)nconns, (int)rv);
	}
	prngmunge();
	while (conns) {
	  /* only the child process listens to these connections */
	  if (conns->closeproc) {
	    conns->closeproc(conns);
	  }
	  ct = conns;
	  conns = ct->next;
	  close(ct->sok);
	  if (ct->label) { free(ct->label); }
	  free(ct);
	  --nconns;
	}
      }
    }
  }
}
