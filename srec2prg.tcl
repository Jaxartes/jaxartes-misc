#!/usr/bin/tclsh
# Jeremy Dilatush - January 2016 - srec2prg.tcl
# Based on some work I did in 2015.
# Convert 6502 machine language program from Motorola SREC format (the output of
# 'crasm') into input commands to a Commodore PRG file (two bytes of
# address followed by binary data).  Stdin to stdout.

# As it currently stands this program only handles byte data (ie 8 bits wide).

proc usage {} {
    puts stderr "Usage: tclsh srec2prg.tcl \[options\] < input > output"
    puts stderr "Options:"
    puts stderr "\tfi= uninitialized byte value, def 0"
    exit 1
}

# read command line parameters into array $parm()
set parm(fi) 0

foreach arg $argv {
    set ap [split $arg =]
    set ap0 [lindex $ap 0]
    set ap1 [lindex $ap 1]
    if {[llength $ap] != 2 ||
        ![string is integer -strict $ap1] ||
        ![info exists parm($ap0)]} {
        puts stderr "Invalid parameter $arg"
        usage
    }
    set parm($ap0) $ap1
}

# initialize a record of the whole memory address space, $rom()
# initialize a record of how many times each address was written, $ovr()
for {set i 0} {$i < 65536} {incr i} {
    set rom($i) $parm(fi)
    set ovr($i) 0
}

# parse1 - pass 1 of parsing an S-Record.  Breaks it into pieces, returned
# in a list:
#       type (including "S")
#       address
#       data
# it validates the checksum & doesn't return it.  The address value it
# returns is a single numeric value; the data value it returns is a
# list of numeric byte values.
#
# Returns empty list on recoverable parse error or a type of record that
# is ignored.
proc parse1 {sr} {
    set l [string length $sr]
    if {$l < 2} {
        error "Record is truncated in its type field"
    }
    set type [string range $sr 0 1]
    if {$l < 4} {
        error "$type record is truncated in its record length field"
    }
    set reclen [scan [string range $sr 2 3] %x]
    switch -- $type {
        S0 { set alen 4 }
        S1 { set alen 4 }
        S2 { set alen 6 }
        S3 { set alen 8 }
        S5 { set alen 4 }
        S6 { set alen 6 }
        S7 { set alen 8 }
        S8 { set alen 6 }
        S9 { set alen 4 }
        default { return [list] }
    }
    if {$l < $alen + 4} {
        error "$type record is truncated in its address part"
    }
    set addr [scan [string range $sr 4 $alen+3] %x]
    set reclend [expr {$reclen * 2 + 4}]
    if {$l != $reclen * 2 + 4} {
        error \
            "$type record has wrong length, encoded $reclend ($reclen) real $l"
    }

    set cksum [scan [string range $sr end-1 end] %x]
    set cksum2 0
    for {set i 2} {$i < $l - 2} {incr i 2} {
        set byte [scan [string range $sr $i $i+1] %x]
        set cksum2 [expr {($cksum2 + $byte) & 255}]
    }
    set cksum2 [expr {$cksum2 ^ 255}]
    if {$cksum != $cksum2} {
        error "$type record bad checksum, got $cksum exp $cksum2"
    }

    set data [list]
    for {set i [expr {$alen + 4}]} {$i < ($l - 2)} {incr i 2} {
        lappend data [scan [string range $sr $i $i+1] %x]
    }

    return [list $type $addr $data]
}

# read the file line by line
set lc_blank 0 ; # skipped lines: blank
set lc_nons 0 ; # skipped lines: no "S"
set lc_parse 0 ; # skipped lines: basic parse error, or unknown type
set lc_nop 0 ; # skipped lines: a type we do nothing with
set lc_use 0 ; # lines not skipped
while {![eof stdin]} {
    # get a line
    set line [string trim [gets stdin]]
    if {$line eq ""} {
        # skip this line
        incr lc_blank
        continue
    }
    if {[string index $line 0] ne "S"} {
        # skip this line
        incr lc_nons
        continue
    }

    # parse it
    set pl [parse1 $line]

    if {![llength $pl]} {
        # skip this line
        incr lc_parse
        continue
    }

    lassign $pl Type Addr Data

    # handle it
    switch -- $Type {
        S1 -
        S2 -
        S3 {
            # Data record
            set l [llength $Data]
            if {$Addr + $l > 65536 || $Addr < 0} {
                error "Address out of range: Record addr $Addr len $l"
            }
            for {set i 0} {$i < $l} {incr i} {
                set ma [expr {$Addr + $i}]
                set rom($ma) [lindex $Data $i]
                incr ovr($ma)
            }
            incr lc_use
        }
        default {
            # skip this line
            incr lc_nop
            continue
        }
    }
}

# Our output must be a contiguous sequence of addresses; our input might not;
# solve that.
set minad ""
set maxad ""
for {set i 0} {$i < 65536} {incr i} {
    if {$ovr($i)} {
        if {$minad eq ""} { set minad $i }
        set maxad $i
    }
}
if {$minad eq ""} {
    error "Input is empty!"
}

# Print a report on what we did
set bf 0
set bu 0
set bo 0
for {set i 0} {$i < 65536} {incr i} {
    switch -- $ovr($i) {
        0 { incr bu }
        1 { incr bf }
        default { incr bf ; incr bo }
    }
}
foreach {rl rv} [list \
    "Skipped lines (blank)" $lc_blank \
    "Skipped lines (not 'S')" $lc_nons \
    "Skipped lines (parse/type)" $lc_parse \
    "Skipped lines (ignored)" $lc_nop \
    "Used lines" $lc_use \
    "Bytes filled" $bf \
    "Bytes unfilled" $bu \
    "Bytes overwritten" $bo \
    "Minimum address" [format "%u ($%x)" $minad $minad] \
    "Maximum address" [format "%u ($%x)" $maxad $maxad]
] {
    puts stderr [format {srec2prg:%27s %s} $rl $rv]
}

# Now write the output.
fconfigure stdout -encoding binary -translation binary
puts -nonewline [format %c%c [expr {$minad & 255}] [expr {($minad >> 8) & 255}]]
for {set i $minad} {$i <= $maxad} {incr i} {
    puts -nonewline [format %c $rom($i)]
}
exit 0
