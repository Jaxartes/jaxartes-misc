/*
 * lx_timer_test_mod.c
 * Test timers in a kernel thread on Linux.
 *
 * While the module is loaded, a thread appears in 'ps' and runs timers.
 * It pseudorandomly chooses the length of time to wait and the timer to
 * use, and then waits.
 *
 * Rather quick-and-dirty. It is after all just a test.
 *
 * To build and run (after installing kernel headers):
 *      make
 *      sudo insmod lx_timer_test_mod.ko
 *      # wait a while
 *      dmesg > File
 *
 * Licensed under BSD or GPL, see bottom of file.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>

#define MY_NAME "lx_timer_test"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Jeremy Dilatush");
MODULE_DESCRIPTION("Test timers in a kernel thread");
MODULE_VERSION("1");

/* parameters that can be set at time of module load */
static u64 min_wait_ns = 0;
static u64 max_wait_ns = 1000000000;
static unsigned minstd_state = 1;
MODULE_PARM_DESC(min_wait_ns, "Shortest wait time in nanoseconds");
MODULE_PARM_DESC(max_wait_ns, "Longest wait time in nanoseconds");
MODULE_PARM_DESC(minstd_state, "Seed for pseudorandom number generator");

/* other variables and prototypes */
static struct task_struct *lx_timer_test_task;

static int lx_timer_test_main(void *);
static unsigned minstd(unsigned *);
static u64 minstd_long_range(u64, u64);

/* is run after loading the module */
static int __init lx_timer_test_mod_init(void)
{
    struct task_struct *task;

    if (max_wait_ns < min_wait_ns ||
        (max_wait_ns >> 42) > 0) {
        printk(KERN_ERR MY_NAME
               ": Bad min_wait_ns/max_wait_ns parameter values\n");
        return(-EINVAL);
    }

    /* churn the pseudorandom number generation state */
    minstd(&minstd_state);
    minstd(&minstd_state);
    minstd(&minstd_state);
    if (minstd_state == 0) {
        printk(KERN_ERR MY_NAME ": Bad minstd_state parameter value\n");
        return(-EINVAL);
    }

    /* start the test thread, which does all the work */
    task = kthread_run(lx_timer_test_main, NULL, MY_NAME);
    if (IS_ERR(task)) {
        printk(KERN_ERR MY_NAME ": Failed to create lx_timer_test thread.\n");
        return(PTR_ERR(task));
    }
    lx_timer_test_task = task;

    return(0);
}

/* is run before unloading the module */
static void __exit lx_timer_test_mod_fini(void)
{
    /* terminate the test thread */
    printk(KERN_ERR "lx_timer_test_mod_fini() starts\n");
    if (lx_timer_test_task) {
        kthread_stop(lx_timer_test_task);
        lx_timer_test_task = NULL;
        printk(KERN_ERR "lx_timer_test_mod_fini() stopped task\n");
    }
    printk(KERN_ERR "lx_timer_test_mod_fini() ends\n");
}

/* main loop of the test thread */
static int lx_timer_test_main(void * unused)
{
    u64 sleepns, slept;
    long sleepj;
    ktime_t sleepk, tbefore, tafter;
    int whichsleep;
    char *how;

    if (kthread_should_stop()) {
        return(0);
    }

    for (;;) {
        /* How long to sleep? The double random scaling is to favor low vals. */
        sleepns = max_wait_ns;
        sleepns = minstd_long_range(min_wait_ns, sleepns);
        sleepns = minstd_long_range(min_wait_ns, sleepns);

        /* Which sleep to use */
        whichsleep = minstd(&minstd_state) % 2;

        /* And sleep */
        switch (whichsleep) {
        case 0:
            sleepj = nsecs_to_jiffies(sleepns);
            how = "schedule_timeout_interruptible";
            printk(KERN_INFO MY_NAME
                   ": about to sleep %lld ns using"
                   " schedule_timeout_interruptible(%ld)\n",
                   (long long)sleepns, (long)sleepj);
            tbefore = ktime_get();
            schedule_timeout_interruptible(sleepj);
            break;
        case 1:
        default:
            sleepk = ns_to_ktime(sleepns);
            how = "schedule_hrtimeout";
            printk(KERN_INFO MY_NAME
                   ": about to sleep %lld ns using schedule_hrtimeout()\n",
                   (long long)sleepns);
            tbefore = ktime_get();
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_hrtimeout(&sleepk, HRTIMER_MODE_REL);
            break;
        }

        /* When did that end? */
        tafter = ktime_get();

        /* If we're supposed to exit, do so */
        if (kthread_should_stop()) {
            printk(KERN_ERR "lx_timer_test_main() exiting");
            break;
        }

        /* Log the time it took */
        slept = ktime_to_ns(ktime_sub(tafter, tbefore));
        printk(KERN_INFO MY_NAME
               ": slept %lld ns planned %lld ns extra %lld ns using %s\n",
               (long long)slept,
               (long long)sleepns,
               (long long)(slept - sleepns),
               how);
    }

    return(0);
}

/* pseudorandom number generator (MINSTD -- Park and Miller 1988 and 1993) */
static unsigned minstd(unsigned *state)
{
    /* I'm being uptight and inefficient in using 'long long' here. */
    long long s = *state;
    s *= 48271;
    s = (s & 0x7fffffffll) ^ (s >> 31);
    *state = (unsigned)s;
    return(*state);
}

static u64 minstd_long_range(u64 mn, u64 mx)
{
    if (mx <= mn) {
        return(mn);
    }
    return(mn + (((mx - mn + 1) *
                  (minstd(&minstd_state) & 0xfffff)) >> 20));
}

module_init(lx_timer_test_mod_init);
module_exit(lx_timer_test_mod_fini);

/*
 * Some informational sources used in writing the above:
 *  Linux kernel sources, of course
 *  https://blog.sourcerer.io/writing-a-simple-linux-kernel-module-d9dc3762c234
 *  https://www.linuxtopia.org/online_books/Linux_Kernel_Module_Programming_Guide/x323.html
 *  https://en.wikipedia.org/w/index.php?title=Lehmer_random_number_generator&oldid=1100449742
 */

/*

Copyright (C) 2022, Jeremy Dilatush.
All rights reserved.

Redistribution and use in source and binary forms, with or without 
modification, are permitted provided that the following conditions 
are met: 
1. Redistributions of source code must retain the above copyright 
   notice, this list of conditions and the following disclaimer. 
2. Redistributions in binary form must reproduce the above copyright 
   notice, this list of conditions and the following disclaimer in the 
   documentation and/or other materials provided with the distribution. 
3. The name of the author may not be used to endorse or promote products 
   derived from this software without specific prior written permission. 

Alternatively, this software may be distributed under the terms of the 
GNU General Public License ("GPL") version 2 as published by the Free 
Software Foundation. 

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR 
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, 
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT 
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF 
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

