/*
 * tcphammer.c - Jeremy Dilatush - 14-15 Jan 2024
 * Use under BSD license, see below.
 *
 * A test program that opens and closes connections to an ECHO protocol
 * (RFC 862), often all at once. For usage details see usage().
 */

/*
 * Status: Written, seems to run ok. Not tested as well as I'd usually do,
 * but it's just for testing.
 *
 * Since it's just for testing, don't run on important production machines.
 * Or anywhere the bugs it no doubt has might unacceptably impact security
 * or utility.
 *
 * Has some kind of problem on macOS (and probably FreeBSD) where it gets
 * EPIPE a lot. The problem is not seen on Linux.
 */
/*
 * tcphammer.c - Jeremy Dilatush
 *
 * Copyright (C) 2024, Jeremy Dilatush.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

void usage(void)
{
    fputs("USAGE:\n"
          "tcphammer takes no parameters.\n"
          "It reads a file on stdin that tells it what to do.\n"
          "Lines, given by example:\n"
          "    #...\n"
          "        comments; ignored\n"
          "    c50/127.0.0.1/11011\n"
          "        50 connection slots: to 127.0.0.1 TCP port 1011. Can be\n"
          "        repeated to connect to different things. Optionally\n"
          "        followed by \"/\" and a name used in reporting.\n"
          "    i5.0\n"
          "        typical interval between actions in seconds\n"
          "    s5/0/4\n"
          "        Control the scale of actions, ultimately what proportions\n"
          "        of connections or connection slots to act on. At least\n"
          "        two integers:\n"
          "            number of fractions to choose\n"
          "            one or more order factors among those (counted from 0)\n"
          "        In the example, 5/0/4 means it picks 5 random numbers in\n"
          "        the range 0-1; sorts them; takes either the first (0)\n"
          "        or last (4); and selects that fraction of connections.\n"
          "    kopendata -- send data immediately after open\n"
          "    kclosedata -- send data immediately before close\n"
          "    ksilentdata -- don't report successful data\n"
          "    kverbose -- detailed reporting for debug purposes\n"
          "    p...\n"
          "        Relative probability of the various actions.\n"
          "        They are:\n"
          "            pd15 -- Data exchange; action is to send data to\n"
          "                selected open connections.\n"
          "            po5 -- Open; action is to open connections on\n"
          "                selected not yet open connection slots.\n"
          "            pc5 -- Close; action is to close connections that\n"
          "                are open and not selected\n"
          "            pt1 -- Toggle; both Open (of selected, not yet open\n"   
          "                slots) and Close (of unselected, open ones)\n"
          "    t60.0\n"
          "        Send/receive timeout in seconds.\n"
          , stderr);
    exit(1);
}

struct cslot {
    /* connection slot */

    /* configuration */
    int                     cs_num;     /* slot number */
    struct sockaddr_storage cs_adr;     /* remote address & port */
    int                     cs_adr_len; /* length of cs_adr */
    char                    cs_name[128];/* name used in reporting */

    /* connection state */
    int                     cs_sok;     /* socket if connected, -1 otherwise */
    unsigned char           cs_data[16];/* data we expect to get back */
    int                     cs_dlen;    /* number of bytes in cs_data[] */
    int                     cs_dpos;    /* where we are matching cs_data[] */

    /* thread, and communication with it */
    pthread_t               cs_thread;  /* thread that serves this connection */
    pthread_mutex_t         cs_lock;
    unsigned char           cs_cbuf[512];
    int                     cs_cmd;
    pthread_cond_t          cs_wake;
    /*
     * How the above are used for communication with the thread:
     *      cs_lock is a lock covering cs_cmd, cs_cbuf, and cs_wake
     *      cs_cbuf contains a command or response
     *      cs_cmd identifies a command or response
     *          == 0: nothing
     *          > 0: response, in cs_cbuf, of given length
     *          < 0: command:
     *              -'d' to send data
     *              -'o' to open
     *              -'c' to close
     *              (there is no toggle command; instead it's open or close)
     *              cs_cbuf will contain 8 bytes of pseudorandom data for use by
     *              the thread.
     *      cs_wake will wake up the thread if it's waiting for a command
     */

    /* other */
    int                     cs_is_open; /* main thread considers it open */
};
#define NEGCHAR(c) (-(int)(c))

struct cslot *cslots;           /* all configured connection slots */
int ncslots;                    /* number of configured connection slots */
int acslots;                    /* number of allocated entries in cslots[] */
const int slot_limit = 5000;    /* max number of slots to allow */
pthread_cond_t main_wake;       /* to wake up the main thread */
pthread_mutex_t main_wake_mutex;/* used with main_wake */
float interval = 5.0;           /* interval between actions */
int scale_nrand;                /* scale control: number of random values */
int *scale_choices;             /* scale control: which ones to choose */
int scale_nchoices;             /* size of scale_choices */
const int rand_limit = 5000;    /* max value of scale_nrand */
int opt_opendata, opt_closedata, opt_verbose; /* option flags */
int opt_silentdata;             /* more option flags */
float prob_data = 15;           /* probability of operation: data */
float prob_open = 5;            /* probability of operation: open */
float prob_close = 5;           /* probability of operation: close */
float prob_toggle = 1;          /* probability of operation: toggle */
struct timeval rtimeo = { 60, 0 }; /* send/receive timeout */

char *timediff(struct timeval *t1, struct timeval *t2, char *buf, int sz)
{
    long long us1, us2, dus;
    char *sgn = "";
    us1 = t1->tv_sec;
    us1 *= 1000000;
    us1 += t1->tv_usec;
    us2 = t2->tv_sec;
    us2 *= 1000000;
    us2 += t2->tv_usec;
    dus = us2 - us1;
    if (dus < 0) {
        dus = -dus;
        sgn = "-";
    }
    buf[0] = '\0';
    buf[sz - 1] = '\0';
    snprintf(buf, sz, "%s%lu.%03u",
             sgn, (unsigned long)(dus / 1000000),
             (unsigned)((dus % 1000000) / 1000));
    return(buf);
}

char *timeshow(struct timeval *t, char *buf, int sz)
{
    struct tm tm;
    char fbuf[64];
    time_t tt = t->tv_sec;

    memset(&tm, 0, sizeof(tm));
    localtime_r(&tt, &tm);
    snprintf(fbuf, sizeof(fbuf), "%%Y-%%m-%%dt%%H:%%M:%%S.%03u",
             (unsigned)(t->tv_usec / 1000));
    buf[0] = '\0';
    buf[sz - 1] = '\0';
    strftime(buf, sz, fbuf, &tm);
    return(buf);
}

/* float_compare() -- comparator for qsort() to compare to floats */
int float_compare(const void *x, const void *y)
{
    float xx = *(const float *)x;
    float yy = *(const float *)y;
    if (xx < yy) return -1;
    if (xx > yy) return 1;
    return 0;
}

int parse_config_slots (char *line)
{
    /*
     * 3 or 4 components separated by "/":
     *      number of slots
     *      IPv4 or IPv6 address
     *      TCP port number
     *      optional label string
     */
    char *cp;
    int count, i, j;
    struct cslot *slot, *template;
    char abuf[64];
    struct sockaddr_in *sinp;
    struct sockaddr_in6 *sin6p;

    /* first argument: count */
    cp = line + 1;
    count = atoi(cp);
    cp += strcspn(cp, "/");
    if (*cp == '/') {
        ++cp;
    } else {
        fprintf(stderr, "Too few parts in '%s'\n", line);
        return(-1);
    }

    /* allocate space for that many connection slots */
    if (count < 1) {
        fprintf(stderr, "Slot count must be positive\n");
        return(-1);
    }
    if (count + ncslots > slot_limit) {
        fprintf(stderr, "Max number of connection slots is %d\n",
                (int)slot_limit);
        return(-1);
    }
    if (count + ncslots > acslots) {
        acslots = count + ncslots;
        acslots += acslots >> 2;
        acslots += 3;
        cslots = realloc(cslots, acslots * sizeof(cslots[0]));
        if (!cslots) {
            fprintf(stderr, "Memory allocation problem.\n");
            return(-1);
        }
    }
    slot = template = &(cslots[ncslots]);
    memset(slot, 0, sizeof(*slot));
    slot->cs_sok = -1;
    slot->cs_is_open = 0;
    slot->cs_dlen = slot->cs_dpos = 0;
    slot->cs_cmd = 0;
    slot->cs_num = ncslots;
    ++ncslots;

    /* second argument: IPv4 or IPv6 address */
    i = strcspn(cp, "/");
    j = (i < sizeof(abuf)) ? i : (sizeof(abuf) - 1);
    strncpy(abuf, cp, j);
    abuf[j] = '\0';
    if (strchr(abuf, ':')) {
        /* IPv6 */
        sinp = NULL;
        sin6p = (struct sockaddr_in6 *)&(slot->cs_adr);
        sin6p->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, abuf, &(sin6p->sin6_addr)) <= 0) {
            fprintf(stderr, "Error parsing address '%s'\n", abuf);
            return(-1);
        }
#ifdef USE_SIN_LEN
        sin6p->sin_len = sizeof(*sin6p);
#endif
        slot->cs_adr_len = sizeof(*sin6p);
    } else {
        /* IPv4 */
        sinp = (struct sockaddr_in *)&(slot->cs_adr);
        sin6p = NULL;
        sinp->sin_family = AF_INET;
        if (inet_pton(AF_INET, abuf, &(sinp->sin_addr)) <= 0) {
            fprintf(stderr, "Error parsing address '%s'\n", abuf);
            return(-1);
        }
#ifdef USE_SIN_LEN
        sinp->sin_len = sizeof(*sinp);
#endif
        slot->cs_adr_len = sizeof(*sinp);
    }
    cp += i;
    if (*cp == '/') {
        ++cp;
    } else {
        fprintf(stderr, "Too few parts in '%s'\n", line);
        return(-1);
    }

    /* third argument: TCP port */
    i = atoi(cp);
    if (sinp)   sinp->sin_port = htons(i);
    if (sin6p)  sin6p->sin6_port = htons(i);
    cp += strcspn(cp, "/");

    /* fourth argument, optional: name */
    if (*cp == '/') {
        ++cp;
        snprintf(slot->cs_name, sizeof(slot->cs_name), "%s", cp);
    } else {
        snprintf(slot->cs_name, sizeof(slot->cs_name), "%s/%d", abuf, i);
    }

    /* make however many copies of the slot structure are desired */
    for (i = 1; i < count; ++i) {
        slot = &(cslots[ncslots]);
        memcpy(slot, template, sizeof(*slot));
        slot->cs_num = ncslots;
        ++ncslots;
    }

    return(0);
}

int parse_config_scale_control(char *line)
{
    /*
     * Two or more integers: Number of fractions to choose, and then some
     * order factors to choose among them. Parsed into scale_nrand and
     * scale_choices.
     */
    char *cp;
    int slashes, i;

    cp = line + 1;
    slashes = 0;
    for (;;) {
        cp += strcspn(cp, "/");
        if (*cp == '/') {
            ++cp;
            ++slashes;
        } else {
            break;
        }
    }
    if (slashes < 1) {
        fprintf(stderr, "Too few parts in scale control spec\n");
        return(-1);
    }
    scale_choices = calloc(slashes, sizeof(scale_choices[0]));
    scale_nchoices = slashes;

    cp = line + 1;
    scale_nrand = atoi(cp);
    if (scale_nrand < 1 || scale_nrand > rand_limit) {
        fprintf(stderr, "First part of scale control must be 1-%d not %d\n",
                (int)rand_limit, (int)scale_nrand);
        return(-1);
    }
    if (opt_verbose) {
        fprintf(stderr, "#scale_nrand = %d, scale_nchoices = %d\n",
                (int)scale_nrand, (int)scale_nchoices);
    }
    for (i = 0; i < slashes; ++i) {
        cp += strcspn(cp, "/");
        if (*cp == '/') ++cp;
        scale_choices[i] = atoi(cp);
        if (scale_choices[i] < 0) {
            fprintf(stderr, "Scale control must be all nonnegative integers"
                    " not %d\n", (int)scale_choices[i]);
            return(-1);
        }
        if (scale_choices[i] >= scale_nrand) {
            fprintf(stderr, "In scale control the first number must be"
                    " biggest; %d >= %d\n",
                    (int)scale_choices[i], (int)scale_nrand);
            return(-1);
        }
    }

    return(0);
}

int parse_config(FILE *fp)
{
    char line[512];
    int ch, len = 0;
    float prob, f;

    for (;;) {
        ch = fgetc(fp);
        if (ch < 0 || ch == '\n') {
            /* got a line */
            while (len > 0 && isspace(line[len - 1])) {
                line[--len] = '\0';
            }
            if (opt_verbose) {
                fprintf(stderr, "# config line: %s\n", line);
            }
            switch (line[0]) {
            case '\0': /* empty line: ignore */
                break;
            case '#': /* comment: ignore this line */
                break;
            case 'c': /* connection slot(s) */
                if (parse_config_slots(line) < 0) {
                    return(-1);
                }
                break;
            case 'i': /* set interval */
                interval = atof(line + 1);
                if (!(interval >= 0 && interval <= 86400)) {
                    fprintf(stderr, "Interval %f out of range 0-86400\n",
                            interval);
                    return(-1);
                }
                break;
            case 's': /* set up scale control */
                if (parse_config_scale_control(line) < 0) {
                    return(-1);
                }
                break;
            case 'k': /* keyword options */
                if (!strcasecmp(line + 1, "opendata")) {
                    opt_opendata = 1;
                } else if (!strcasecmp(line + 1, "closedata")) {
                    opt_closedata = 1;
                } else if (!strcasecmp(line + 1, "silentdata")) {
                    opt_silentdata = 1;
                } else if (!strcasecmp(line + 1, "verbose")) {
                    opt_verbose = 1;
                } else {
                    fprintf(stderr, "Unknown option keyword '%s'\n", line + 1);
                    return(-1);
                }
                break;
            case 'p': /* relative probabilities */
                if (line[1] == '\0') {
                    fprintf(stderr, "Truncated 'p' line\n");
                    return(-1);
                }
                prob = atof(line + 2);
                if (!(prob >= 0)) {
                    fprintf(stderr, "Probabilities must be nonnegative\n");
                    return(-1);
                }
                switch (line[1]) {
                case 'd': prob_data = prob; break;
                case 'o': prob_open = prob; break;
                case 'c': prob_close = prob; break;
                case 't': prob_toggle = prob; break;
                default:
                    fprintf(stderr, "Unknown action probability '%c'\n",
                            (int)line[1]);
                    return(-1);
                }
                break;
            case 't': /* send/receive timeout in seconds */
                f = atof(line + 1);
                if (!(f > 0 && f <= 86400)) {
                    fprintf(stderr, "Timeout must be"
                            " positive and no more than 86400, not %f\n", f);
                    return(-1);
                }
                rtimeo.tv_sec = floor(f);
                f -= rtimeo.tv_sec;
                f *= 1e+6;
                rtimeo.tv_usec = floor(f);
                break;
            default:
                fprintf(stderr, "Unknown configuration class '%c'\n",
                        (int)line[0]);
                return(-1);
            }
            len = 0;
        } else {
            if (len + 1 >= sizeof(line)) {
                fprintf(stderr, "Input line too long\n");
                return(-1);
            }
            line[len++] = ch;
        }
        line[len] = '\0';
        if (ch < 0) {
            break;
        }
    }

    /* validation and normalization */
    if (ncslots < 1) {
        fprintf(stderr, "Must set up at least one connection.\n");
        return(-1);
    }
    if (scale_nrand == 0) {
        fprintf(stderr, "Missing scale control setting\n");
        return(-1);
    }
    prob = prob_data + prob_open + prob_close + prob_toggle;
    prob_data /= prob;
    prob_open /= prob;
    prob_close /= prob;
    prob_toggle /= prob;

    if (opt_verbose) {
        fprintf(stderr, "# config has been read\n");
    }

    return(0);
}

/* main loop for a thread handling each connection slot */
void *cslot_main(void *vp)
{
    struct cslot *slot = vp;
    int cmd;
    unsigned char cbuf[8], buf2[8];
    struct timeval tstart, tend;
    char msg[512], *opstr, tbuf1[64], tbuf2[64], tbuf3[64];
    int err, port, len, sent, got, rv;
    struct sockaddr_storage addr;
    socklen_t alen;

    for (;;) {
        /* wait until we have a command */
        pthread_mutex_lock(&(slot->cs_lock));
        while (slot->cs_cmd >= 0) {
            pthread_cond_wait(&(slot->cs_wake), &(slot->cs_lock));
        }

        /* get the command */
        cmd = slot->cs_cmd;
        memcpy(cbuf, slot->cs_cbuf, 8);
        pthread_mutex_unlock(&(slot->cs_lock));

        /* perform the command */
        gettimeofday(&tstart, NULL);
        snprintf(msg, sizeof(msg), "ok");
        err = 0;
        if (cmd == NEGCHAR('o')) {
            /* open connection */
            if (slot->cs_sok >= 0) {
                /* already open, that's unreasonable but ok */
                snprintf(msg, sizeof(msg), "was already open");
            } else {
                slot->cs_sok = socket(slot->cs_adr.ss_family,
                                      SOCK_STREAM, IPPROTO_TCP);
                if (slot->cs_sok < 0) {
                    /* was not open, that's unreasonable but ok */
                    snprintf(msg, sizeof(msg), "was already closed");
                    /* some kind of error */
                    snprintf(msg, sizeof(msg), "open: %s", strerror(errno));
                    err = 1;
                }
                if (slot->cs_sok >= 0) {
                    setsockopt(slot->cs_sok, SOL_SOCKET, SO_RCVTIMEO,
                               &rtimeo, sizeof(rtimeo));
                    setsockopt(slot->cs_sok, SOL_SOCKET, SO_SNDTIMEO,
                               &rtimeo, sizeof(rtimeo));
                }
                if (slot->cs_sok >= 0 &&
                    connect(slot->cs_sok,
                            (struct sockaddr *)&(slot->cs_adr),
                            slot->cs_adr_len) < 0) {
                    /* some kind of error */
                    snprintf(msg, sizeof(msg), "connect: %s", strerror(errno));
                    err = 1;
                    slot->cs_sok = -1;
                } else {
                    /* success */
                    memset(&addr, 0, sizeof(addr));
                    alen = sizeof(addr);
                    if (getsockname(slot->cs_sok, (void *)&addr, &alen) < 0) {
                        /* unable to get local socket info, but that's ok */
                        port = 0;
                    } else {
                        if (addr.ss_family == AF_INET) {
                            struct sockaddr_in *a4 = (void *)&addr;
                            port = ntohs(a4->sin_port);
                        }
                        else if (addr.ss_family == AF_INET6) {
                            struct sockaddr_in6 *a6 = (void *)&addr;
                            port = ntohs(a6->sin6_port);
                        }
                        else {
                            port = 0;
                        }
                    }
                    snprintf(msg, sizeof(msg), "connected %d->%s",
                             (int)port, slot->cs_name);
                }
            }
        }
        if (cmd == NEGCHAR('d') ||
            (opt_opendata && cmd == NEGCHAR('o') && slot->cs_sok >= 0) ||
            (opt_closedata && cmd == NEGCHAR('c') && slot->cs_sok >= 0)) {
            if (slot->cs_sok < 0) {
                /* was not open, that's unreasonable but ok */
                snprintf(msg, sizeof(msg), "was not open");
            } else {
                /* send data and wait for a reply */
                len = (cbuf[7] % 7) + 1;
                sent = write(slot->cs_sok, cbuf, len);
                if (sent < 0) {
                    /* some kind of error */
                    snprintf(msg, sizeof(msg), "write: %s", strerror(errno));
                    err = 1;
                } else {
                    if (sent > len) { sent = len; } /* should be impossible */
                    got = 0;
                    while (got < sent) {
                        rv = read(slot->cs_sok, buf2 + got, len);
                        if (rv < 0) {
                            /* some kind of error */
                            snprintf(msg, sizeof(msg),
                                     "read: %s", strerror(errno));
                            err = 1;
                            break;
                        } else if (rv == 0) {
                            /* end of input */
                            snprintf(msg, sizeof(msg), "connection was closed");
                            err = 1;
                            close(slot->cs_sok);
                            slot->cs_sok = -1;
                            break;
                        } else {
                            /* got some response */
                            got += rv;
                        }
                    }
                    if (memcmp(cbuf, buf2, got) != 0) {
                        /* response mismatch */
                        snprintf(msg, sizeof(msg), "response did not match");
                        err = 1;
                    } else if (err) {
                        /* error, message already set */
                    } else {
                        /* good response */
                        if (cmd == NEGCHAR('d')) {
                            snprintf(msg, sizeof(msg), "good %d byte exchange",
                                     (int)got);
                        }
                    }
                }
            }
        }
        if (cmd == NEGCHAR('c')) {
            /* close connection */
            if (slot->cs_sok < 0) {
                /* was not open, that's unreasonable but ok */
                snprintf(msg, sizeof(msg), "was not open");
            } else {
                if (close(slot->cs_sok) < 0) {
                    /* some kind of error */
                    snprintf(msg, sizeof(msg), "close: %s", strerror(errno));
                    err = 1;
                } else {
                    /* success */
                    snprintf(msg, sizeof(msg), "closed");
                }
                slot->cs_sok = -1;
            }
        }

        /*
         * Provide a report about the result of the command. The report
         * is CSV formatted and contains the following fields:
         *      0. slot number
         *      1. duration of operation in seconds
         *      2. time operation began (numeric)
         *      3. time operation ended (numeric)
         *      4. time operation began (human readable)
         *      5. time operation ended (human readable)
         *      6. string identifying operation
         *      7. string describing slot
         *      8. short string describing result: "ok" or "err"
         *      9. long string describing result
         */
        pthread_mutex_lock(&(slot->cs_lock));
        gettimeofday(&tend, NULL);
        opstr = "?";
        if (cmd == NEGCHAR('o')) { opstr = "open"; }
        if (cmd == NEGCHAR('c')) { opstr = "close"; }
        if (cmd == NEGCHAR('d')) { opstr = "data"; }
        if (opt_silentdata && cmd == NEGCHAR('d') && !err) {
            /* user doesn't want to know every time we exchange a few bytes */
            slot->cs_cmd = 0;
            slot->cs_cbuf[0] = '\0';
        } else {
            slot->cs_cmd = snprintf((char *)slot->cs_cbuf, sizeof(slot->cs_cbuf),
                                    "%d,%s,%u.%03u,%u.%03u,%s,%s,%s,%s,%s,\"%s\"",
                                    slot->cs_num,
                                    timediff(&tstart, &tend, tbuf1, sizeof(tbuf1)),
                                    (unsigned)tstart.tv_sec,
                                    (unsigned)(tstart.tv_usec / 1000),
                                    (unsigned)tend.tv_sec,
                                    (unsigned)(tend.tv_usec / 1000),
                                    timeshow(&tstart, tbuf2, sizeof(tbuf2)),
                                    timeshow(&tend, tbuf3, sizeof(tbuf3)),
                                    opstr,
                                    slot->cs_name,
                                    err ? "err" : "ok",
                                    msg);
        }
        if (slot->cs_cmd >= sizeof(slot->cs_cbuf)) {
            slot->cs_cmd = sizeof(slot->cs_cbuf) - 1;
        }
        pthread_mutex_unlock(&(slot->cs_lock));

        /* and wake up the main thread if it was waiting for us */
        pthread_mutex_lock(&main_wake_mutex);
        pthread_cond_signal(&main_wake);
        pthread_mutex_unlock(&main_wake_mutex);
    }
}

int main(int argc, char **argv)
{
    int i, j, e, action, slotaction;
    struct cslot *slot;
    struct timeval tnow;
    struct timespec tnext;
    float use_interval, r, *rs;
    int nopen = 0;

    signal(SIGPIPE, SIG_IGN);

    /* command line */
    if (argc != 1) {
        usage();
    }
    srand48(time(NULL)); /* not a secure way to do it, but ok for this */

    /* configuration file: see usage() for details */
    if (parse_config(stdin) < 0) {
        exit(1);
    }
    rs = calloc(scale_nrand, sizeof(rs[0]));

    /* start threads */
    if (pthread_mutex_init(&main_wake_mutex, NULL) ||
        pthread_cond_init(&main_wake, NULL)) {
        fprintf(stderr, "Failed to allocate Pthread mutex or condition var\n");
        exit(1);
    }
    for (i = 0; i < ncslots; ++i) {
        slot = &(cslots[i]);
        if (pthread_mutex_init(&(slot->cs_lock), NULL) ||
            pthread_cond_init(&(slot->cs_wake), NULL)) {
            fprintf(stderr,
                    "Failed to allocate Pthread mutex or condition var\n");
            exit(1);
        }
        e = pthread_create(&(slot->cs_thread), NULL, &cslot_main, slot);
    }
    if (opt_verbose) {
        fprintf(stderr, "# threads have been started\n");
    }

    /* main loop */
    for (;;) {
        /* figure out when we'll next do an action */
        gettimeofday(&tnow, NULL);
        tnext.tv_sec = tnow.tv_sec;
        tnext.tv_nsec = tnow.tv_usec;
        tnext.tv_nsec *= 1000;
        use_interval = interval * (drand48() + drand48());
        i = floor(use_interval);
        use_interval -= i;
        use_interval *= 1e+9;
        tnext.tv_sec += i;
        tnext.tv_nsec += floor(use_interval);
        if (tnext.tv_nsec >= 1000000000) {
            tnext.tv_nsec -= 1000000000;
            tnext.tv_sec++;
        }
        if (opt_verbose) {
            fprintf(stderr,
                    "# top of main loop; now = %lu.%06u, next = %lu.%09u\n",
                    (unsigned long)tnow.tv_sec, (unsigned)tnow.tv_usec,
                    (unsigned long)tnext.tv_sec, (unsigned)tnext.tv_nsec);
        }

        /* handle responses, and wait, until it's time for action */
        for (;;) {
            if (opt_verbose) {
                fprintf(stderr, "# waiting...\n");
            }
            pthread_mutex_lock(&main_wake_mutex);
            e = pthread_cond_timedwait(&main_wake, &main_wake_mutex, &tnext);
            if (e == ETIMEDOUT) {
                /* It's time to do an action */
                pthread_mutex_unlock(&main_wake_mutex);
                if (opt_verbose) {
                    fprintf(stderr, "# it's time!\n");
                }
                break;
            } else {
                /* got a response, handle it */
                if (opt_verbose) {
                    fprintf(stderr, "# got response(s)!\n");
                }
                /*
                 * Going through the list of slots to find out which one
                 * has a response now is an inefficient way to do it.
                 * But anything better seems rather more complicated,
                 * so I'm going to try it this way.
                 */
                pthread_mutex_unlock(&main_wake_mutex);
                for (i = 0; i < ncslots; ++i) {
                    slot = &(cslots[i]);
                    pthread_mutex_lock(&(slot->cs_lock));
                    if (slot->cs_cmd > 0) {
                        printf("%.*s\n", (int)slot->cs_cmd, slot->cs_cbuf);
                        slot->cs_cmd = 0;
                    }
                    pthread_mutex_unlock(&(slot->cs_lock));
                }
                fflush(stdout);
            }
        }

        /* time for action */

        /* choose the action */
        r = drand48();
        if (r < prob_data) {
            action = 'd';
        } else {
            r -= prob_data;
            if (r < prob_open) {
                action = 'o';
            } else {
                r -= prob_open;
                if (r < prob_toggle) {
                    action = 't';
                } else {
                    action = 'c';
                }
            }
        }
        if (!nopen) {
            /* nothing we can do if we don't have connections open */
            action = 'o';
        }

        /* select a subset of connection slots; tell the threads handling
         * them to do their thing
         */
        for (i = 0; i < scale_nrand; ++i) {
            rs[i] = drand48();
        }
        qsort(rs, scale_nrand, sizeof(rs[0]), &float_compare);
        r = rs[scale_choices[lrand48() % scale_nchoices]];
        if (opt_verbose) {
            fprintf(stderr, "# action %c selection probability %f\n",
                    action, r);
        }
        for (i = 0; i < ncslots; ++i) {
            slot = &(cslots[i]);
            slotaction = action;
            pthread_mutex_lock(&(slot->cs_lock));
            if (slot->cs_cmd > 0) {
                /* we're still waiting for it to do the last thing */
                pthread_mutex_unlock(&(slot->cs_lock));
                continue;
            }
            if (drand48() < r) {
                /* selected: action on data, open; inaction on close */
                if (action == 'd' && slot->cs_is_open) {
                    /* ok */
                } else if (action == 'o' && !slot->cs_is_open) {
                    slot->cs_is_open = 1;
                    ++nopen;
                } else if (action == 't' && !slot->cs_is_open) {
                    /* toggle, when closed, means open */
                    slotaction = 'o';
                    slot->cs_is_open = 1;
                    ++nopen;
                } else {
                    slotaction = '\0';
                }
            } else {
                /* unselected: which means action on close */
                if (action == 'c' && slot->cs_is_open) {
                    slot->cs_is_open = 0;
                    --nopen;
                } else if (action == 't' && slot->cs_is_open) {
                    slotaction = 'c'; /* toggle, when open, means close */
                    slot->cs_is_open = 0;
                    --nopen;
                } else {
                    slotaction = '\0';
                }
            }
            if (slotaction != '\0') {
                /* yeah, we're doing something, and here it is */
                if (opt_verbose) {
                    fprintf(stderr, "# command to slot %d: %c\n",
                            i, slotaction);
                }
                slot->cs_cmd = -slotaction;
                for (j = 0; j < 8; ++j) {
                    slot->cs_cbuf[j] = lrand48() & 255;
                }
                pthread_cond_signal(&(slot->cs_wake));
            }
            pthread_mutex_unlock(&(slot->cs_lock));
        }
    }
}
