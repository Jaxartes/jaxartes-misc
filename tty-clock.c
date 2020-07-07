/*
 * tty-clock.c - Jeremy Dilatush
 *
 * Copyright (C) 2020, Jeremy Dilatush.
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
 * tty-clock - Display a clock and calendar on the terminal.
 * Bugs:
 *      + The output isn't quite right for non-English-speaking locales.
 *      This might be addressed by setting better 'fmt' values
 *      in date_widget_redraw() but portability and implementation of the
 *      the -h & -s options could be a problem.
 *      + The banner output doesn't show an indication of AM / PM when
 *      used with the -h option.
 *
 * Additional compile time options:
 *     -DCNCTEST -- enable some code for testing calculate_next_change()
 *     -DFTCTEST -- enable some code for testing fake_time_calc()
 *     -DRAW -- use 'raw' mode for input instead of 'cbreak'
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <libgen.h>
#include <locale.h>
#include <curses.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/select.h>
#ifdef RAW
#include <signal.h>
#endif

static char *progname = "tty-clock";

#define MIN_DELAY_USEC  200000 /* shortest delay: 1/5 second */
#define MAX_DELAY_SEC   172800 /* longest delay: 2 days */

static void usage(void)
{
    fprintf(stderr, "USAGE:\n"
            "    %s [options]\n"
            "OPTIONS:\n"
            "    -r num -- Run at num times realtime; default 1.\n"
            "    -o (+/-)time -- Add time seconds to the time the program\n"
            "                    is started (to display a different time).\n"
            /*
             * It'd be nice to have a better way to specify time than
             * that, but date input is a pain.  The "+"/"-" prefix is
             * included to make room for a later addition to the input
             * format.
             */
            "    -h -- show 12 hour instead of 24 hour time\n"
            "    -H -- make the blocks in the banner halftone not solid\n"
            "    -s -- suppress seconds\n"
            "    -b -- suppress display of banner-sized time\n"
            "    -c -- suppress display of 3-month calendar\n"
            "    -d -- suppress display of plain date+time line\n"
            , progname);
    exit(1);
}

/* debugging output (-D) */

static FILE *dbg = NULL; /* output file for debugging */

#define dbgf(args) if (dbg != NULL) { dbgf_ args ; }

static void dbgf_(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(dbg, fmt, ap);
    va_end(ap);
    fputc('\n', dbg);
    fflush(dbg);
}

static char *dbg_timestamp(char *buf, size_t bufsz, struct timeval *tv)
{
    struct tm lt;
    size_t len;

    localtime_r(&tv->tv_sec, &lt);
    len = strftime(buf, bufsz, "%Y-%m-%d-%H:%M:%S", &lt);
    if (len <= 0 || len >= bufsz) {
        return("?"); /* shouldn't happen */
    }
    snprintf(buf + len, bufsz - len, ".%.06lu", (unsigned long)tv->tv_usec);

    return(buf);
}

/* "fake" time calculation */

struct fake_time_control {
    /* values for enabling and controlling fake time */
    int enable;                 /* flag enabling fake time calculation */
    struct timeval orig;        /* "origin" all calculations are relative to */
    double scale;               /* scaling factor */
    double offset;              /* add/subtract time in seconds */
};

/* fake_time_calc(): Alter time 't' from real to fake time. */
static void fake_time_calc(struct timeval *t, struct fake_time_control *ftc)
{
    double x, y;

    /* adjust the seconds */
    x = t->tv_sec;
    x -= ftc->orig.tv_sec;
    x *= ftc->scale;
    x += ftc->offset;
    t->tv_sec = floor(x);
    x -= t->tv_sec;
    t->tv_sec += ftc->orig.tv_sec;

    /* adjust the microseconds */
    y = t->tv_usec;
    y -= ftc->orig.tv_usec;
    y *= ftc->scale;
    y += x * 1e+6;
    y += ftc->orig.tv_usec;
    x = rint(y);

    /* canonicalize those */
    y = floor(x / 1e+6);
    t->tv_sec += y;
    t->tv_usec = x - y * 1e+6;
}

/* display "widgets" making up part of the screen */

/* generic-ish "widget" structure */
struct widget {
    void *data; /* type specific data */
    WINDOW *ww; /* where displayed */
    int rowmn, rowmx; /* row number range */
    char *name; /* label for debugging */
    /* There could be a column number range, but not needed until/unless
     * there are some widgets spaced horizontally.
     */
    /* change_by -- will display change by specified time?
     *              (NULL means - every second) */
    /* redraw -- redraw for specified time */
    int (*change_by)(struct widget *w, time_t t, struct tm *tm);
    void (*redraw)(struct widget *w, time_t t, struct tm *tm, int faked_time);
    time_t last_drawn; /* time it was last drawn */
    struct tm last_drawn_d; /* details of last_drawn */
    int opt_nosec, opt_12h; /* some option flags multiple widgets use */
};

static void generic_widget_init(struct widget *w, WINDOW *ww,
                                int *row, int nrows)
{
    memset(w, 0, sizeof(*w));
    w->ww = ww;
    w->rowmn = *row;
    *row += nrows;
    w->rowmx = (*row) - 1;
    w->name = "?";
    w->opt_nosec = w->opt_12h = 0;
    /* caller should set w->data, w->change_by, w->redraw, w->name;
     * and set w->opt_* as desired */
}

/* "date" widget: single line of output resembling the "date" command */

static int date_widget_change_by(struct widget *w, time_t t, struct tm *tm)
{
    /* only used when w->opt_nosec == 0, ie not displaying seconds */
    struct tm *tm0 = &(w->last_drawn_d);
    return(tm->tm_min  != tm0->tm_min  || tm->tm_hour != tm0->tm_hour ||
           tm->tm_yday != tm0->tm_yday || tm->tm_year != tm0->tm_year);
}

static void date_widget_redraw(struct widget *w, time_t t, struct tm *tm,
                               int faked_time)
{
    char *fmt;
    char buf[96];

    if (w->opt_nosec) {
        if (w->opt_12h) {
            fmt = "%a %b %e %l:%M %p %Z %Y";
        } else {
            fmt = "%a %b %e %k:%M %Z %Y";
        }
    } else {
        if (w->opt_12h) {
            fmt = "%a %b %e %l:%M:%S %p %Z %Y";
        } else {
            fmt = "%a %b %e %k:%M:%S %Z %Y";
        }
    }

    strftime(buf, sizeof(buf), fmt, tm);
    buf[sizeof(buf) - 1] = '\0';
    dbgf(("        %s() new string %s", __FUNCTION__, buf));

    mvwaddstr(w->ww, w->rowmn, 0, buf);
    if (faked_time) {
        waddstr(w->ww, " (Fake time)");
    }
    wclrtoeol(w->ww);
}

static void date_widget_init(struct widget *w, WINDOW *ww,
                             int *row, int nosec, int opt_12h)
{
    generic_widget_init(w, ww, row, 1);
    w->data = NULL;
    w->change_by = nosec ? &date_widget_change_by : NULL;
    w->redraw = &date_widget_redraw;
    w->name = "date";
    w->opt_nosec = nosec;
    w->opt_12h = opt_12h;
}

/* "banner" widget: banner sized time display */

struct banner_widget_font {
    /* A font for use by the banner widget: 12 character glyphs
     * represented in bitmap.  They all have the same height and
     * each has its own width. 
     *
     * Because each "pixel" in the banner display
     * is itself a character, its aspect ratio is already right and
     * e.g. 8x8 fonts are ideal.
     *
     * The character codes are:
     *      0-9 - for those digits
     *      10 - space
     *      11 - colon
     *
     * Uses the rightmost bits of each bitmap word, with the leftmost
     * bit being the leftmost pixel.  (Because that's how the first font
     * data I used has it.)
     */
    int height; /* pixel height of all glyphs */
    int widths[12]; /* pixel width of each glyph */
    u_int32_t *bitmap; /* one entry per row */
};

static u_int32_t banner_widget_font_1_bitmap[] = {
    /* 8x8 font from IBM PC BIOS, with extra row padding on top */
    /* https://ibm.retropc.se/bios/BIOS_5150_27OCT82_U33.BIN offset 0x1a6e */

    /* 0  "0" 48 0x1bee */ 0x00, 0x7c, 0xc6, 0xce, 0xde, 0xf6, 0xe6, 0x7c, 0x00,
    /* 1  "1" 49 0x1bf6 */ 0x00, 0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0xfc, 0x00,
    /* 2  "2" 50 0x1bfe */ 0x00, 0x78, 0xcc, 0x0c, 0x38, 0x60, 0xcc, 0xfc, 0x00,
    /* 3  "3" 51 0x1c06 */ 0x00, 0x78, 0xcc, 0x0c, 0x38, 0x0c, 0xcc, 0x78, 0x00,
    /* 4  "4" 52 0x1c0e */ 0x00, 0x1c, 0x3c, 0x6c, 0xcc, 0xfe, 0x0c, 0x1e, 0x00,
    /* 5  "5" 53 0x1c16 */ 0x00, 0xfc, 0xc0, 0xf8, 0x0c, 0x0c, 0xcc, 0x78, 0x00,
    /* 6  "6" 54 0x1c1e */ 0x00, 0x38, 0x60, 0xc0, 0xf8, 0xcc, 0xcc, 0x78, 0x00,
    /* 7  "7" 55 0x1c26 */ 0x00, 0xfc, 0xcc, 0x0c, 0x18, 0x30, 0x30, 0x30, 0x00,
    /* 8  "8" 56 0x1c2e */ 0x00, 0x78, 0xcc, 0xcc, 0x78, 0xcc, 0xcc, 0x78, 0x00,
    /* 9  "9" 57 0x1c36 */ 0x00, 0x78, 0xcc, 0xcc, 0x7c, 0x0c, 0x18, 0x70, 0x00,
    /* 10 " " just blank*/ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 11 ":" 58 0x1c3e */ 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00,
};

static struct banner_widget_font banner_widget_font_1 = {
    /* 8x8 font from IBM PC BIOS, with extra row padding on top */
    9, { 8, 8, 8, 8, 8,   8, 8, 8, 8, 8,   8, 8 }, banner_widget_font_1_bitmap
};

struct banner_widget {
    /* 'data' field of 'struct widget' for a banner widget */
    struct banner_widget_font *fnt; /* font to use */
    chtype mark, space;
};

static int banner_widget_change_by(struct widget *w, time_t t, struct tm *tm)
{
    /* only used when w->opt_nosec == 0, ie not displaying seconds */
    struct tm *tm0 = &(w->last_drawn_d);
    return(tm->tm_min  != tm0->tm_min  || tm->tm_hour != tm0->tm_hour ||
           tm->tm_yday != tm0->tm_yday || tm->tm_year != tm0->tm_year);
}

static void banner_widget_redraw(struct widget *w, time_t t, struct tm *tm,
                                 int faked_time)
{
    struct banner_widget *bw = w->data;
    struct banner_widget_font *fnt = bw->fnt;
    int y, x, c, h;
    int str[8], len;

    /* represent the time in hh:mm:ss or hh:mm format using our own
     * character codes
     */
    len = 0;
    h = tm->tm_hour;
    if (w->opt_12h) {
        h %= 12;
        if (h == 0) h = 12;
    }
    str[len++] = (h / 10) ? (h / 10) : 10; /* space or 1-2 */ 
    str[len++] = h % 10; /* 0-9 */
    str[len++] = 11; /* : */
    str[len++] = tm->tm_min / 10; /* 0-5 */
    str[len++] = tm->tm_min % 10; /* 0-9 */
    if (!w->opt_nosec) {
        str[len++] = 11; /* : */
        str[len++] = tm->tm_sec / 10; /* 0-5 */
        str[len++] = tm->tm_sec % 10; /* 0-9 */
    }

    /* and now draw that time, banner sized: one character per "pixel" */
    for (y = 0; y < fnt->height; ++y) {
        wmove(w->ww, w->rowmn + y, 0);
        for (c = 0; c < len; ++c) {
            for (x = fnt->widths[str[c]] - 1; x >= 0; --x) {
                waddch(w->ww,
                       ((fnt->bitmap[str[c] * fnt->height + y] >> x) & 1) ?
                       bw->mark : bw->space);
            }
        }
    }
}

static void banner_widget_init(struct widget *w, WINDOW *ww,
                               int *row,
                               int nosec, int opt_12h, int opt_halftone,
                               struct banner_widget_font *fnt)
{
    struct banner_widget *bw;

    generic_widget_init(w, ww, row, fnt->height);
    w->change_by = nosec ? &banner_widget_change_by : NULL;
    w->redraw = &banner_widget_redraw;
    w->name = "banner";
    w->data = bw = calloc(1, sizeof(*bw));
    bw->fnt = fnt;
    w->opt_nosec = nosec;
    w->opt_12h = opt_12h;
    bw->mark = opt_halftone ? ACS_CKBOARD : (' ' | A_REVERSE);
    /* ACS_BLOCK didn't show up well on mine, using ' ' | A_REVERSE instead */
    bw->space = ' ';
}

/* "cal" widget: 3 month calendar */

static int cal_widget_change_by(struct widget *w, time_t t, struct tm *tm)
{
    struct tm *tm0 = &(w->last_drawn_d);
    dbgf(("        %s(): %d/%d -> %d/%d",
          __FUNCTION__,
          (int)tm0->tm_year, (int)tm->tm_year,
          (int)tm0->tm_yday, (int)tm->tm_yday));

    return((tm->tm_year != tm0->tm_year) || (tm->tm_yday != tm0->tm_yday));
}

static void cal_widget_redraw(struct widget *w, time_t t, struct tm *tm,
                              int faked_time)
{
    int rm;             /* relative month 0-2 being drawn */
    int mo;             /* actual month 0-11 being drawn */
    int ye;             /* year corresponding to mo */
    struct tm tmt;      /* temporary time data */
    time_t rdt;         /* representing the day we're displaying now */
    int rdd;            /* day of the month we're displaying now */
    int rdy;            /* line of screen for day we're displaying now */
    int rdtoday;        /* is the day we're displaying now, today? */
    char monyear[32];   /* buffer holding text to write: month & year */
    chtype dayshow[2];  /* for showing the day */
    int rmx[3] = { 0, 22, 44 }; /* horizontal positions of month displays */
    int mow = 20;       /* horizontal width of a month display */
    int y;
    size_t sz;

    /* clear the area occupied by the widget */
    for (y = w->rowmn; y <= w->rowmx; ++y) {
        wmove(w->ww, y, 0);
        wclrtoeol(w->ww);
    }

    /* draw the three months */
    for (rm = 0; rm < 3; ++rm) {
        /* figure out actual month and year for this month display */
        mo = tm->tm_mon + rm - 1;
        ye = tm->tm_year + 1900;
        if (mo < 0) {
            mo += 12;
            ye -= 1;
        } else if (mo > 11) {
            mo -= 12;
            ye += 1;
        }

        /* get info for noon the first day of the month */
        tmt = *tm;                  /* just as a sane initialization value */
        tmt.tm_sec = 0;
        tmt.tm_min = 0;
        tmt.tm_hour = 12;
        tmt.tm_mday = 1;
        tmt.tm_mon = mo;
        tmt.tm_year = ye - 1900;
        /* tmt.tm_wday & tmt.tm_yday will be filled in by mktime() */
        tmt.tm_isdst = -1;
        tmt.tm_zone = NULL;
        rdd = 1;
        rdt = mktime(&tmt);
        rdy = w->rowmn + 2;

        /* display the month and year header line, centered */
        sz = strftime(monyear, sizeof(monyear), "%B %Y", &tmt);
        mvwaddstr(w->ww, w->rowmn, rmx[rm] + ((mow - sz) >> 1), monyear);

        /* display a day of the week guide on the next line */
        mvwaddstr(w->ww, w->rowmn + 1, rmx[rm], "Su Mo Tu We Th Fr Sa");

        /* display days of the month until we run out of days */
        for (;;) {
            /* get info about this day */
            localtime_r(&rdt, &tmt);
            rdtoday = (tmt.tm_year == tm->tm_year &&
                       tmt.tm_yday == tm->tm_yday);

            /* see about weeks and the end of the month */
            if (tmt.tm_mday < rdd) {
                /* new month, this one is done */
                dbgf(("Month '%s' ends: has no day %d",
                      monyear, (int)rdd));
                break;
            }
            if (tmt.tm_wday == 0 && rdd != 1) {
                /* new week, new line on the screen */
                rdy += 1;
            }
            if (rdy > w->rowmx) {
                /* out of space, this month must be done */
                dbgf(("Month '%s' ends: out of weeks at day %d",
                      monyear, (int)rdd));
                break;
            }

            /* display the day info in the appropriate place */
            dbgf(("Month '%s' day %d: y=%d x=%d highlight=%s",
                  monyear, (int)rdd, (int)rdy, (int)(rmx[rm] + tmt.tm_wday * 3),
                  rdtoday ? "yes" : "no"));
            wmove(w->ww, rdy, rmx[rm] + tmt.tm_wday * 3);
            if (tmt.tm_mday < 10) {
                dayshow[0] = ' ';
            } else {
                dayshow[0] = '0' + (tmt.tm_mday / 10);
            }
            dayshow[1] = '0' + (tmt.tm_mday % 10);
            if (rdtoday) {
                dayshow[0] |= WA_STANDOUT;
                dayshow[1] |= WA_STANDOUT;
            }
            waddch(w->ww, dayshow[0]);
            waddch(w->ww, dayshow[1]);

            /* next day */
            rdd++;
            rdt += 86400;
        }
    }

    /* Example from 'cal' of the kind of output being imitated:
              May 2020             June 2020             July 2020
        Su Mo Tu We Th Fr Sa  Su Mo Tu We Th Fr Sa  Su Mo Tu We Th Fr Sa
                        1  2      1  2  3  4  5  6            1  2  3  4
         3  4  5  6  7  8  9   7  8  9 10 11 12 13   5  6  7  8  9 10 11
        10 11 12 13 14 15 16  14 15 16 17 18 19 20  12 13 14 15 16 17 18
        17 18 19 20 21 22 23  21 22 23 24 25 26 27  19 20 21 22 23 24 25
        24 25 26 27 28 29 30  28 29 30              26 27 28 29 30 31
        31
        w/ highlight on current day's two digits or space and digit
     */
}

static void cal_widget_init(struct widget *w, WINDOW *ww, int *row)
{
    generic_widget_init(w, ww, row, 8);
    w->change_by = &cal_widget_change_by;
    w->redraw = &cal_widget_redraw;
    w->name = "cal";
}

/* Calculate the time to the next change, calling all widgets'
 * 'change_by' handlers.  They all have to have 'change_by' handlers or
 * this function can't run, but is also unnecessary (just use 'tnow+1').
 *
 * One could argue this is overcomplicated when usually updates will
 * happen at fixed intervals (second, 60 seconds, 1 day = 86400).  But
 * there's more to it (daylight saving time lengthens one day, shortens
 * another).  Also it's nice to write code that could still work if we
 * add some widget with trickier timing in the future.
 */
time_t calculate_next_change(time_t tnow, struct widget *widgets,
                             int num_widgets)
{
    int i, j;
    const int max_future = 17; /* log base 2 of how many seconds in the
                                * future we even consider */
    time_t tmin;            /* lowest possible return value */
    time_t tmax;            /* highest possible return value */
    time_t tmid;            /* in between */
    struct tm tm;           /* decoded time */

    dbgf(("calculate_next_change(%lu)", (unsigned long)tnow));

    /* Start out trying to find an upper bound, a power of 2 seconds from
     * now that either there's been a change by (on at least one widget)
     * or that we give up looking ('max_future').
     */
    tmin = tmax = tnow;
    for (j = 0; j <= max_future; ++j) {
        tmin = tmax;
        tmax = tnow + (1UL << j);
        dbgf(("    tmin=%lu tmax=%lu (j=%d)",
              (unsigned long)tmin,
              (unsigned long)tmax, (int)j));
        localtime_r(&tmax, &tm);
        for (i = 0; i < num_widgets; ++i) {
            if (widgets[i].change_by(&(widgets[i]), tmax, &tm)) {
                break; /* this one changed */
            }
        }
        if (i < num_widgets) {
            break; /* one of them changed */
        }
    }

    if (j == 0) {
        return(tmax); /* changes in the next second */
    }

    /* At this point we know that:
     *      j > 1
     *      tnow < tmin < tmax
     *      change_by(tmax) is true (or we shall pretend it is) for some widget
     *      change_by(tmin) is not true for any widget
     * Those last two will be maintained in the loop which follows, as
     * we do a binary search narrowing down the range.
     */
    for (;;) {
        /* see about the midpoint */
        dbgf(("    tmin=%lu tmax=%lu",
              (unsigned long)tmin,
              (unsigned long)tmax));

        tmid = (tmin + tmax) >> 1;
        if (tmid <= tmin) {
            /* they've converged; done */
            dbgf(("    tmax=%lu", (unsigned long)tmax));
            return(tmax);
        }

        /* has it changed by tmid? */
        localtime_r(&tmid, &tm);
        for (i = 0; i < num_widgets; ++i) {
            if (widgets[i].change_by(&(widgets[i]), tmid, &tm)) {
                break; /* this one changed */
            }
        }
        if (i < num_widgets) {
            /* yes: changed by tmid */
            tmax = tmid;
        } else {
            /* no: didn't change by tmid */
            tmin = tmid;
        }
    }

    return(tmax);
}

#ifdef CNCTEST
/* tester for calculate_next_change(): time t, should change at ct */
static int cnctest_cb(struct widget *w, time_t t, struct tm *tm)
{
    int ct = *(int *)w->data;
    return(t >= ct);
}
static void cnctest(int t, int ct)
{
    struct widget w;
    int nc;

    /* make a fake "widget" */
    memset(&w, 0, sizeof(w));
    w.data = &ct;
    w.name = "cnctest";
    w.change_by = &cnctest_cb;

    /* run it */
    nc = calculate_next_change(t, &w, 1);

    /* report result */
    printf("cnctest(%d, %d) ==> %d\n", (int)t, (int)ct, (int)nc);
}
#endif /* CNCTEST */

#ifdef FTCTEST
/* tester for fake_time_calc() */
static void ftctest(struct fake_time_control *ftc)
{
    /* output is in CSV format to the -D debug file with the following
     * columns:
     *      "ftctest" to identify these lines
     *      origin time (in seconds since 1970, including fractional part)
     *      scale
     *      offset
     *      input time (in seconds since 1970, including fractional part)
     *      output time (in seconds since 1970, including fractional part)
     */
    int e, s, f;
    struct timeval tv, ftv;

    for (s = -1; s < 2; ++s) {
        for (e = 0; e < 25; ++e) {
            if (s == 0 && e != 0) {
                continue; /* skip: zero is just zero */
            }
            if (fabs((1ull << e) * ftc->scale) > 1e+9) {
                continue; /* skip: pushes arithmetic range */
            }
            for (f = 0; f < 19; ++f) {
                tv.tv_sec = ftc->orig.tv_sec + (s << e);
                tv.tv_usec = f * 54321;
                ftv = tv;
                fake_time_calc(&ftv, ftc);
                dbgf(("ftctest,%lu.%06lu,%le,%le,%lu.%06lu,%lu.%06lu",
                      (unsigned long)ftc->orig.tv_sec,
                      (unsigned long)ftc->orig.tv_usec,
                      (double)ftc->scale,
                      (double)ftc->offset,
                      (unsigned long)tv.tv_sec,
                      (unsigned long)tv.tv_usec,
                      (unsigned long)ftv.tv_sec,
                      (unsigned long)ftv.tv_usec));
            }
        }
    }
}
#endif /* FTCTEST */

/* main program */

int main(int argc, char **argv)
{
    int oc, row, num_widgets, ch, i;
    struct widget widgets[3], *w;
    int opt_12h = 0, opt_nosec = 0, opt_halftone = 0;
    int opt_noban = 0, opt_nocal = 0, opt_nodate = 0;
    WINDOW *ww;
    struct fake_time_control fake_time; /* see -o, -r options */
    struct timeval tlast;   /* time shown by last display update */
    time_t tnext;           /* next time we'll redraw */
    struct timeval tnow;    /* present time */
    struct timeval treal;   /* present time - real */
    struct timeval dly;     /* time to wait */
    struct tm tnow_d;       /* details of tnow */
    int draw_all = 1;       /* forces (re)drawing of whole display */
    int waited = 1;         /* waited since last drawing? */
    int every_second = 0;   /* display changes every second */
    fd_set rfds;
    char tsbuf[64], tsbuf2[64];

    /* initial initialization */

    if (gettimeofday(&fake_time.orig, NULL) < 0) {
        perror("gettimeofday");
        /* if we can't tell what time it is then a clock is pointless */
        return(1);
    }

    tlast = fake_time.orig; /* a sane dummy value for now */
    tnext = 0;
    fake_time.enable = 0;
    fake_time.scale = 1;
    fake_time.offset = 0;

    if (argc > 0) {
        progname = basename(argv[0]);
    }

    /* parse the command line options */

    while ((oc = getopt(argc, argv, "r:o:hsbcdHD:")) >= 0) {
        switch (oc) {
        case 'r': /* -r num -- time rate */
            fake_time.enable = 1;
            fake_time.scale = atof(optarg);
            if (!isfinite(fake_time.scale) || fake_time.scale < 0) {
                fprintf(stderr, "%s: Invalid time scale '%s'\n",
                        progname, optarg);
                usage();
            }
            break;
        case 'o': /* -o (+/-)seconds -- time offset */
            fake_time.enable = 1;
            if (optarg[0] == '+' || optarg[0] == '-') {
                fake_time.offset = atof(optarg);
                if (!isfinite(fake_time.offset)) {
                    fprintf(stderr, "%s: Invalid time offset '%s'\n",
                            progname, optarg);
                    usage();
                }
            } else {
                fprintf(stderr, "%s: Invalid time offset '%s'\n",
                        progname, optarg);
                usage();
            }
            break;
        case 'h': /* -h -- 12 hour time */
            opt_12h = 1;
            break;
        case 's': /* -s -- suppress seconds */
            opt_nosec = 1;
            break;
        case 'b': /* -b -- suppress banner */
            opt_noban = 1;
            break;
        case 'c': /* -c -- suppress calendar */
            opt_nocal = 1;
            break;
        case 'd': /* -d -- suppress plain date+time line */
            opt_nodate = 1;
            break;
        case 'H': /* -H -- halftone blocks for the banner */
            opt_halftone = 1;
            break;
        case 'D': /* -D -- write debugging info to tty-clock-debug-out */
            dbg = fopen(optarg, "a");
            if (!dbg) {
                fprintf(stderr, "%s: %s: %s\n",
                        progname, optarg, strerror(errno));
                return(1);
            }
            dbgf(("Starting: %s\n",
                  dbg_timestamp(tsbuf, sizeof(tsbuf), &fake_time.orig)));
            break;
        default:
            fprintf(stderr, "%s: Invalid option flag.\n", progname);
            usage();
        }
    }

#ifdef CNCTEST
    if (argc == optind + 2) {
        /* test calculate_next_change() instead of doing the clock thing */
        cnctest(atoi(argv[optind]), atoi(argv[optind + 1]));
        return(0);
    }
#endif /* CNCTEST */

#ifdef FTCTEST
    ftctest(&fake_time);
#endif /* FTCTEST */

    if (optind < argc) {
        fprintf(stderr, "%s: Too many arguments.\n", progname);
        usage();
    }

    dbgf(("Command line options parsed."));

    /* initialize curses display */

    setlocale(LC_ALL, "");
    ww = initscr();
#ifdef RAW
    raw();
#else
    cbreak();
#endif
    noecho();
    nonl();
    intrflush(stdscr, FALSE);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    /* build the widgets */

    row = num_widgets = 0;
    if (!opt_nodate) {
        /* single line like the output of "date" */
        date_widget_init(&(widgets[num_widgets++]), ww, &row,
                         opt_nosec, opt_12h);
    }
    if (!opt_noban) {
        /* banner-sized time display */
        banner_widget_init(&(widgets[num_widgets++]), ww, &row,
                           opt_nosec, opt_12h, opt_halftone,
                           &banner_widget_font_1);
    }
    if (!opt_nocal) {
        /* three month calendar */
        cal_widget_init(&(widgets[num_widgets++]), ww, &row);
    }

    /* does any widget lack a change_by handler & thus update every second? */
    for (i = 0; i < num_widgets; ++i) {
        if (widgets[i].change_by == NULL) {
            every_second = 1;
            break;
        }
    }

    /* now keep updating the display, with delays until it should change */

    for (;;) {
        dbgf(("Top of event loop"));

        /* See if there are any interesting keys typed on the keyboard */
        while ((ch = getch()) != ERR) {
            /* key pressed, see what interesting thing happens */
            /* keys recognized:
             *      ^C, q - end program
             *      ^L - redraw screen
             */
            dbgf(("    keypress: %d\n", (int)ch));
            switch (ch) {
            case 12:                /* ^L - redraw screen */
            case KEY_NPAGE:
            case KEY_CLEAR:
                dbgf(("    key action: redraw screen"));
                draw_all = 1;   
                break;
#ifdef RAW
            case 3:                 /* ^C, q - end program */
            case KEY_BREAK:
#endif /* RAW */
            case 'q':
            case 'Q':
                dbgf(("    key action: end program"));
                endwin();
                return(0);
                break;
#ifdef RAW
            case 26:                /* ^Z - suspend program */
            case KEY_SUSPEND:
                dbgf(("    key action: suspend program"));
                raise(SIGSTOP);
                break;
#endif /* RAW */
            default:
                /* ignore unrecognized keys */
                dbgf(("    key action: ignored (unrecognized)"));
                break;
            }
        }
        
        /* Find out what time it is & what time we're going to display */

        if (gettimeofday(&tnow, NULL) < 0) {
            endwin();
            perror("gettimeofday");
            return(1);
        }
        if (fake_time.enable) {
            /* show, not the real time but this adjusted time */
            treal = tnow;
            fake_time_calc(&tnow, &fake_time);
            dbgf(("'fake' time calculated: %s -> %s",
                  dbg_timestamp(tsbuf, sizeof(tsbuf), &treal),
                  dbg_timestamp(tsbuf2, sizeof(tsbuf2), &tnow)));
        } else {
            dbgf(("real time used: %s",
                  dbg_timestamp(tsbuf, sizeof(tsbuf), &tnow)));
        }

        localtime_r(&tnow.tv_sec, &tnow_d);

        /* Is it time to change any part of the display? */
        if (tnow.tv_sec < tlast.tv_sec) {
            dbgf(("time went backwards, redrawing everything"));
            draw_all = 1;
        }

        if ((!draw_all) && (tnow.tv_sec < tnext || !waited)) {
            /* Nope.  Wait until it is */
            double delay;

            delay = tnext - tnow.tv_sec - (((double)tnow.tv_usec) * 1e-6);
            if (fake_time.enable) {
                if (fake_time.scale < 1e-6) {
                    /* special case because division by zero is bad */
                    delay *= 1e+6;
                } else {
                    delay /= fake_time.scale;
                }
            }

            dly.tv_sec = floor(delay);
            dly.tv_usec = rint((delay - dly.tv_sec) * 1e+6);
            if (dly.tv_sec < 0) {
                dly.tv_sec = dly.tv_usec = 0; /* don't do negative delay */
            }
            if (dly.tv_sec == 0 && dly.tv_usec < MIN_DELAY_USEC) {
                dly.tv_usec = MIN_DELAY_USEC; /* don't do super short delay */
            }
            if (dly.tv_sec >= MAX_DELAY_SEC) {
                dly.tv_sec = MAX_DELAY_SEC; /* don't do super long delay */
                dly.tv_usec = 0;
            }
            dbgf(("waiting %u.%06lu seconds unless keypress comes in",
                  (unsigned)dly.tv_sec, (unsigned long)dly.tv_usec));
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            select(STDIN_FILENO + 1, &rfds, NULL, NULL, &dly);
            waited = 1;
            continue;
        }

        /* Figure out what it's time to redraw, and do so */
        if (draw_all) {
            wclear(ww);
        }
        for (i = 0; i < num_widgets; ++i) {
            w = &(widgets[i]);
            if (draw_all ||
                (w->change_by == NULL && w->last_drawn != tnow.tv_sec) ||
                (w->change_by && w->change_by(w, tnow.tv_sec, &tnow_d))) {

                dbgf(("    redrawing widget %d '%s'", (int)i, w->name));
                w->redraw(w, tnow.tv_sec, &tnow_d, fake_time.enable);
                w->last_drawn = tnow.tv_sec;
                w->last_drawn_d = tnow_d;
            }
        }
        refresh();

        /* Figure out when is the next time we'll need to redraw anything */
        if (every_second) {
            tnext = tnow.tv_sec + 1;
        } else {
            tnext = calculate_next_change(tnow.tv_sec, widgets, num_widgets);
        }

        /* Follow-up settings to keep track of what we did */
        draw_all = 0;
        tlast = tnow;
        waited = 0;
    }
}
