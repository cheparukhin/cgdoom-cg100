#!/bin/sh
# Compare two -crc565 files frame by frame (tests/tc-exact.sh).
#
#   crc565-compare.sh A B
#
# Frames marked F (different on purpose, see host -crc565) in either file
# are skipped. Prints the first mismatching frames and a summary; exit
# status 0 if every other frame matches and both files have the same frames.

A=$1
B=$2
[ -f "$A" ] && [ -f "$B" ] || { echo "usage: $0 A B" >&2; exit 2; }

paste -d ' ' "$A" "$B" | awk -v a="$A" '
NF < 6 { short = 1; next }
{
    # $1 frame, $2 tic, $3 crc [, F] then the same for B
    fa = ($4 == "F"); off = fa ? 5 : 4
    ta = $2; fb = ($(off+3) == "F")
    if ($1 != $(off) ) { framemis++; next }
    if (fa || fb) { skipped++; next }
    if ($3 != $(off+2)) { if (bad < 5) print "frame " $1 " tic " $2 ": " $3 " != " $(off+2); bad++ }
    else ok++
}
END {
    printf "%d frames match, %d differ, %d skipped\n", ok, bad, skipped
    if (framemis || short) print "frame lists differ"
    exit (bad || framemis || short) ? 1 : 0
}'
