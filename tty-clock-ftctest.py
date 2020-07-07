#!/usr/bin/python3
# Copyright (C) 2020 Jeremy Dilatush
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY JEREMY DILATUSH AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
# TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL JEREMY DILATUSH OR CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

# Check the output of ftctest() in tty-clock.c, on stdin.
# Takes the 6-column "ftctest" CSV lines, and appends one more column
# to each: error.
# Also writes out, in lines beginning with "#", some summary information.

# Uses integer arithmetic for the sake of precision.

from math import sqrt

# Accumulated state, for summary
worst_err = 0           # worst error (absolute value)
sum_err2 = 0            # sum of squares of error
num_err = 0             # number of lines' errors included in sum_err2

# Fixed point read of times: from fractional seconds to integer microseconds
def usread(s):
    parts = s.split(".")
    if len(parts) < 1:
        return(0) # ok, that's just weird
    t = int(parts[0]) # seconds
    t *= 1000000 # as microseconds
    s = (parts[1] + "00000")[:6] # microseconds 
    t += int(s, base=10)
    return(t)

# Read in the lines, processing any that match.
while True:
    # Get a line
    try:
        line = input()
    except EOFError:
        break

    # Is it one of the "ftctest,..." lines?
    if line[:8] != "ftctest,":
        continue

    # Break it into columns.  Skip if there aren't as many as there should be.
    cols = line.split(",")
    if len(cols) < 6:
        continue

    # And here are the columns.  Name and parse them.
    c_magic, c_orig, c_scale, c_offset, c_input, c_output = cols
    us_orig = usread(c_orig)
    n_scale, d_scale = float.as_integer_ratio(float(c_scale))
    us_offset = int(round(float(c_offset) * 1e+6))
    us_input = usread(c_input)
    us_output = usread(c_output)

    # Calculate the correct value.
    us_calc = ((us_input - us_orig) * n_scale + (d_scale >> 1)) // d_scale
    us_calc = us_calc + us_offset + us_orig

    # Error computation
    err = us_output - us_calc
    err2 = err * err
    erra = abs(err)
    if worst_err < erra: worst_err = erra
    sum_err2 += err2
    num_err += 1

    # Enhanced line
    print(line + "," + str(err))

# Summary of error
print("# " + str(num_err) + " samples.")
print("# " + str(worst_err) + " usec worst error.")
print("# " + str(sqrt(sum_err2 / num_err)) + " usec RMS error.")
