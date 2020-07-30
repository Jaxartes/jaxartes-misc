/*
 * tvalentine.c - Jeremy Dilatush
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
 * tvalentine - Display a Valentine-style heart and a message on the terminal.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <locale.h>
#include <curses.h>

/* image data */

#define IMG_XOFF 16 /* horizontal offset */
#define IMG_XDIM 48 /* horizontal size */
#define IMG_YDIM 24 /* vertical size */
char img_grid[IMG_YDIM][IMG_XDIM+1] = {
    /* see tvalentine_gen.py */
    { "                                                " },
    { "                                                " },
    { "       !!!!!!!!!!              !!!!!!!!!!       " },
    { "    !!!!!!!!!!!!!!!!        !!!!!!!!!!!!!!!!    " },
    { "  !!!!!!!!!!!!!!!!!!!!    !!!!!!!!!!!!!!!!!!!!  " },
    { " !!!!!!!!!!!!!!!!!!!!!!  !!!!!!!!!!!!!!!!!!!!!! " },
    { "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" },
    { "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" },
    { "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" },
    { "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" },
    { " !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! " },
    { "  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!  " },
    { "    !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!    " },
    { "      !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!      " },
    { "        !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!        " },
    { "          !!!!!!!!!!!!!!!!!!!!!!!!!!!!          " },
    { "            !!!!!!!!!!!!!!!!!!!!!!!!            " },
    { "              !!!!!!!!!!!!!!!!!!!!              " },
    { "                !!!!!!!!!!!!!!!!                " },
    { "                  !!!!!!!!!!!!                  " },
    { "                    !!!!!!!!                    " },
    { "                      !!!!                      " },
    { "                                                " },
    { "                                                " },
};
short img_fgcolor = COLOR_WHITE;    /* color of message */
short img_incolor = COLOR_RED;      /* color of shape outside message */
short img_bgcolor = COLOR_BLACK;    /* color outside shape */
char msg[] = " Love! ";
int fill_count = 5; /* copies of msg[] to do at a time */
long fill_delay = 250; /* milliseconds between updates */

struct attrpos {
    /* possible attribute, see curs_addch(3X) */
    chtype ats; /* attribute value(s) */
    int mil; /* probability in units of 1/1000 */
} attrpos[] = {
    { A_UNDERLINE, 300 }, /* doesn't show up on many terminals, too bad */
#ifdef USE_REVERSE
    { A_REVERSE, 150 }, /* unaesthetic IMO */
#endif
    { A_BOLD, 300 },
    { A_DIM, 100 }, /* doesn't show up on many terminals, too bad */
    { 0, -1 }
};

/* wait for character on stdin, or time, or something to happen */

void ezsleep(long ms)
{
    struct pollfd fds[1];

    memset(fds, 0, sizeof(fds));
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    poll(fds, 1, ms);
}

/* main program */

int main(int argc, char **argv)
{
    int y, x, ch, u, msg_len, i, a;
    WINDOW *ww;
    chtype cc, ac;

    /* initial initialization */
    srand48(time(NULL)); /* Not a secure pseudorandom number generator,
                          * but more than sufficient to this purpose.
                          */
    msg_len = strlen(msg);

    /* initialize curses display */

    setlocale(LC_ALL, "");
    ww = initscr();
    start_color();
    if (COLOR_PAIRS < 3) {
        endwin();
        fprintf(stderr, "uh, can't do colors?!\n");
        return(1);
    }
    cbreak();
    noecho();
    nonl();
    nodelay(stdscr, TRUE);
    wclear(ww);
    init_pair(1, img_fgcolor, img_bgcolor); /* color pair 1: outside img */
    init_pair(2, img_fgcolor, img_incolor); /* color pair 2: inside img */

    /* draw the initial image */
    for (y = 0; y < IMG_YDIM; ++y) {
        for (x = 0; x < IMG_XDIM; ++x) {
            cc = ' ';
            cc |= COLOR_PAIR((img_grid[y][x] & 1) + 1);
            mvwaddch(ww, y, x + IMG_XOFF, cc);
        }
    }
    wrefresh(ww);

    /* now keep updating the display, with delays until it should change */

    for (;;) {
        /* See if there are any interesting keys typed on the keyboard */
        while ((ch = getch()) != ERR) {
            /* key pressed, see what interesting thing happens */
            switch (ch) {
            case 'q':
            case 'Q':
                endwin();
                return(0);
                break;
            default:
                /* ignore unrecognized keys */
                break;
            }
        }

        /* Update display using msg[] */
        for (u = 0; u < fill_count; ++u) {
            x = lrand48() % (IMG_XDIM - msg_len);
            y = lrand48() % IMG_YDIM;
            ac = 0;
            for (a = 0; attrpos[a].mil >= 0; ++a) {
                if ((lrand48() % 1000) < attrpos[a].mil) {
                    ac |= attrpos[a].ats;
                }
            }
            for (i = 0; i < msg_len; ++i) {
                cc = msg[i];
                cc |= COLOR_PAIR((img_grid[y][x+i] & 1) + 1);
                mvwaddch(ww, y, x + i + IMG_XOFF, cc | ac);
            }
        }
        wrefresh(ww);

        /* Delay */
        ezsleep(fill_delay);
    }
}

