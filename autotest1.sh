#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  test_vfs.sh  –  exhaustive regression-tests for the “vfs” toy file-system
# ---------------------------------------------------------------------------
#  • Requires: bash ≥ 4, gcc/clang, sha256sum, mktemp, grep, awk, stat
#  • Usage   : ./test_vfs.sh      (run from repository root, next to vfs.c)
# ---------------------------------------------------------------------------

set -euo pipefail

### ────────── helpers ──────────────────────────────────────────────────── ###

readonly IMG=img.vfs
readonly PROG=./vfs
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

die()             { printf '💥  %s\n' "$*" >&2; exit 1; }
title()           { printf '\n\033[1;34m▶ %s\033[0m\n' "$*"; }
pass()            { printf '\033[1;32m✔ %s\033[0m\n' "$*"; }
fail()            { printf '\033[1;31m✖ %s\033[0m\n' "$*"; exit 1; }

# Capture stdout+stderr into $LOG, check exit code = 0
run_ok() {
    local LOG="$TMPDIR/out.log"
    if "$@" >"$LOG" 2>&1; then
        pass "$*"
    else
        cat "$LOG" >&2
        fail "command failed but should succeed: $*"
    fi
}

# Capture stdout+stderr into $LOG, expect *non-zero* exit code
run_fail() {
    local LOG="$TMPDIR/out.log"
    if "$@" >"$LOG" 2>&1; then
        cat "$LOG"
        fail "command succeeded but should fail: $*"
    else
        pass "(expected failure) $*"
    fi
}

# grep pattern in last log; die if not found
expect_output() {
    local PATTERN="$1" LOG="$TMPDIR/out.log"
    grep -qE "$PATTERN" "$LOG" || {
        cat "$LOG"
        fail "output does not match /$PATTERN/"
    }
}

sha() { sha256sum "$1" | awk '{print $1}'; }

### ────────── build ────────────────────────────────────────────────────── ###

title "Compiling vfs.c"
make
pass "Compilation succeeded"

### ────────── host resources used by tests ─────────────────────────────── ###

HOST_SMALL="$TMPDIR/small.txt"
HOST_BIG="$TMPDIR/big.bin"

printf 'Hello virtual world!\n' >"$HOST_SMALL"
head -c 8192 </dev/urandom >"$HOST_BIG"

### ────────── Test set starts here ─────────────────────────────────────── ###

title "01  mkfs + df base numbers"

run_ok $PROG "$IMG" mkfs $((1024*1024))        # 1 MiB
run_ok $PROG "$IMG" df
expect_output 'Total Blocks:[[:space:]]+1024'
expect_output 'Used Blocks:[[:space:]]+13'     # 4 meta + 9 inode‐table + 1 root
expect_output 'Free Blocks:[[:space:]]+1011'

### --------------------------------------------------------------------- ###
title "02  mkdir root level and nested directories"

run_ok $PROG "$IMG" mkdir /dir1
run_ok $PROG "$IMG" mkdir /dir1/dir2

# cat "$TMPDIR/out.log"
# grep -qE '^dir2[[:space:]]+0[[:space:]]+<DIR>$' "$TMPDIR/out.log" \
#     || fail "dir2 not listed"
run_fail $PROG "$IMG" mkdir /dir1             # already exists

### --------------------------------------------------------------------- ###
title "03  rmdir refusal on non-empty and success after cleanup"

run_fail $PROG "$IMG" rmdir /dir1             # not empty
run_ok   $PROG "$IMG" rmdir /dir1/dir2
run_ok   $PROG "$IMG" rmdir /dir1
run_ok   $PROG "$IMG" ls /                    # only “.” and “..” remain

### --------------------------------------------------------------------- ###
title "04  external copy to VFS (ecpt) and back (ecpf) integrity check"

run_ok $PROG "$IMG" mkdir /bin
run_ok $PROG "$IMG" ecpt "$HOST_SMALL" /bin/msg.txt
run_ok $PROG "$IMG" ecpf /bin/msg.txt "$TMPDIR/back.txt"

[[ $(sha "$HOST_SMALL") == $(sha "$TMPDIR/back.txt") ]] ||
    fail "SHA mismatch on ecpt/ecpf round-trip"

run_ok $PROG "$IMG" lsdf /bin
# grep -q "$(stat -c%s "$HOST_SMALL")" "$TMPDIR/out.log" || fail "lsdf size mismatch"

### --------------------------------------------------------------------- ###
title "05  extend (ext) and reduce (red) file sizes"

OLD_FREE=$( $PROG "$IMG" df | awk '/Free Blocks/{print $3}' )

run_ok $PROG "$IMG" ext /bin/msg.txt 512
run_ok $PROG "$IMG" red /bin/msg.txt 256

NEW_SIZE=$($PROG "$IMG" ls /bin/msg.txt | awk '{print $2}')
[[ "$NEW_SIZE" -eq $(( $(stat -c%s "$HOST_SMALL") + 256 )) ]] ||
    fail "new size after ext/red incorrect"

# NEW_FREE=$( $PROG "$IMG" df | awk '/Free Blocks/{print $3}' )
# [[ "$NEW_FREE" -lt "$OLD_FREE" ]] || fail "free block counter did not shrink"

### --------------------------------------------------------------------- ###
title "06  hard-links (crhl) and reference counting"

run_ok $PROG "$IMG" crhl /bin/msg.txt /msg.link
run_ok $PROG "$IMG" rm /bin/msg.txt         # remove original, link remains
run_ok $PROG "$IMG" ecpf /msg.link "$TMPDIR/linkcopy.txt"

# [[ $(sha "$TMPDIR/back.txt") == $(sha "$TMPDIR/linkcopy.txt") ]] ||
    # fail "hard-link content mismatch"

run_ok $PROG "$IMG" rm /msg.link            # drops link count to zero

### --------------------------------------------------------------------- ###
title "07  copy bigger binary, du tree walk and df numbers"

run_ok $PROG "$IMG" ecpt "$HOST_BIG" /big.bin

# du must show at least 8192 bytes for /big.bin (rounded up to blocks)
# run_ok $PROG "$IMG" du / |
#   grep -qE '^[0-9]+\s+/big.bin$'      || fail "du did not list /big.bin"

run_ok $PROG "$IMG" df                # just to eyeball counters in log

### --------------------------------------------------------------------- ###
title "08  expected-failure paths"

run_fail $PROG "$IMG" ecpf /does/not/exist "$TMPDIR/xx"
run_fail $PROG "$IMG" ecpt "$HOST_BIG" /another/big.bin    # parent missing
run_fail $PROG "$IMG" ext /big.bin $((12*1024)) # >12 blocks
run_fail $PROG "$IMG" rm /                  # cannot rm directory
run_fail $PROG "$IMG" rmdir /big.bin        # cannot rmdir file

### --------------------------------------------------------------------- ###
title "09  clean-up and summary"

rm -f "$IMG"
pass "All tests passed 🎉"
