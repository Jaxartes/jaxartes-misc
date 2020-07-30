#!/usr/bin/python3
# tvalentine_gen.py - Jeremy Dilatush
# 
# Copyright (C) 2020, Jeremy Dilatush.
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
# 
# tvalentine_gen - Generate a coarse bitmap for copying into tvalentine.c

# X, Y dimensions in cells
dim = (48, 24)

# circle centers, radii
# in coordinate system where center of upper left cell is (0, 0)
# radii are given in pairs, because character cells are not usually square
circs = (((11.5, 7.5), (12, 6)),
         ((35.5, 7.5), (12, 6)))

# Pointy bit below them, represented as a set of vectors, and values such
# that the dot product of a coordinate with each vector must be <= the
# specified value to be "inside".
poly = ((( 0, -1), -11.75),
        (( 1,  2),  50.5 + 0.707107*24),
        ((-1,  2),  3.5 + 0.707107*24))
    # Not quite right, maybe make a diamond shape?
    # For now just fill in the "gap" by hand in tvalentine.c.

# characters to use: empty and filled
chars = (" ", "!")

# go for it
for y in range(dim[1]):
    line = []
    for x in range(dim[0]):
        fill = False
        # see if it's filled in within any circle
        for ctr, rads in circs:
            xro = (x - ctr[0]) / rads[0]
            yro = (y - ctr[1]) / rads[1]
            if (xro ** 2 + yro ** 2) <= 1:
                fill = True
                break
        # see if it's in the pointy bit below
        if not fill:
            inpoly = True
            for vec, thr in poly:
                dp = vec[0] * x + vec[1] * y
                if dp > thr:
                    inpoly = False
                    break
            fill = inpoly
        # generate it
        if fill:
            line.append(chars[1])
        else:
            line.append(chars[0])
    print("    { \""+("".join(line))+"\" },")

