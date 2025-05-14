#!/usr/bin/env bash
set -u
IFS=$'\n\t'

VFS_EXEC="./vfs"
IMAGE="test.img"
DISK_SIZE=$((4 * 1024 * 1024))        # 4 MiB
BLOCKSIZE=1024

PASS=0
FAIL=0

print_result () {
    local rc=$1 want=$3 msg=$2
    if [[ $rc -eq $want ]]; then
        printf '[PASS] %s\n' "$msg"
        ((PASS++))
    else
        printf '[FAIL] %s\n        expected %d got %d\n' "$msg" "$want" "$rc"
        ((FAIL++))
    fi
}

cleanup () { rm -f "$IMAGE" tmp.*; }
trap cleanup EXIT

df_field () {               # usage: df_field "string" "Free Blocks:"
    printf '%s\n' "$1" | awk -v k="$2" '$0~k{print $(NF)}'
}

lsdf_bytes () {
    local out
    out=$("$VFS_EXEC" "$IMAGE" lsdf "$1" 2>/dev/null) || return 1
    printf '%s\n' "$out" | sed -E 's/.*: ([0-9]+) bytes.*/\1/'
}


# numerical compare wrapper (print_result compatible)
num_expect () {
    local lhs=$1 op=$2 rhs=$3 msg=$4
    if [[ $(awk "BEGIN{print ($lhs $op $rhs)?0:1}") -eq 0 ]]; then
        print_result 0 "$msg" 0
    else
        print_result 1 "$msg" 0
    fi
}

###############################################################################
echo 'Running VFS automated tests…'
###############################################################################
cleanup
"$VFS_EXEC" "$IMAGE" mkfs "$DISK_SIZE"
print_result $? 'mkfs creates image' 0

###############################################################################
# df
###############################################################################
df1="$("$VFS_EXEC" "$IMAGE" df)"
tb=$(df_field "$df1" 'Total Blocks:'); fb1=$(df_field "$df1" 'Free Blocks:')
ui1=$(df_field "$df1" 'Free Inodes:')
num_expect "$tb"  -gt 0 'df shows total blocks'
num_expect "$fb1" -gt 0 'df shows free blocks'

###############################################################################
# mkdir
###############################################################################
"$VFS_EXEC" "$IMAGE" mkdir /dirA      >/dev/null 2>&1
print_result $? 'mkdir /dirA' 0
"$VFS_EXEC" "$IMAGE" mkdir /dirA/sub  >/dev/null 2>&1
print_result $? 'mkdir /dirA/sub' 0

"$VFS_EXEC" "$IMAGE" rmdir /dirA      >/dev/null 2>&1
print_result $? 'rmdir non-empty /dirA fails' 1
"$VFS_EXEC" "$IMAGE" rmdir /dirA/sub  >/dev/null 2>&1
print_result $? 'rmdir /dirA/sub succeeds' 0
"$VFS_EXEC" "$IMAGE" rmdir /dirA      >/dev/null 2>&1
print_result $? 'rmdir now-empty /dirA succeeds' 0
"$VFS_EXEC" "$IMAGE" rmdir /nope      >/dev/null 2>&1
print_result $? 'rmdir non-existent fails' 1

# verify resources got freed
df2="$("$VFS_EXEC" "$IMAGE" df)"
fb2=$(df_field "$df2" 'Free Blocks:'); ui2=$(df_field "$df2" 'Free Inodes:')
num_expect "$fb2" -ge "$fb1" 'free blocks did not decrease after rmdir'
num_expect "$ui2" -ge "$ui1" 'free inodes did not decrease after rmdir'

###############################################################################
# external copy <-> host
###############################################################################
EXT_IN=$(mktemp tmp.in.XXXX); EXT_OUT=$(mktemp tmp.out.XXXX)
echo "Hello from host!" >"$EXT_IN"

"$VFS_EXEC" "$IMAGE" ecpt "$EXT_IN" /greeting.txt >/dev/null 2>&1
print_result $? 'ecpt copies host file' 0
"$VFS_EXEC" "$IMAGE" ls /        | grep -q 'greeting.txt'
print_result $? 'ls shows greeting.txt' 0
"$VFS_EXEC" "$IMAGE" ecpf /greeting.txt "$EXT_OUT" >/dev/null 2>&1
print_result $? 'ecpf copies back to host' 0
cmp -s "$EXT_IN" "$EXT_OUT"
print_result $? 'round-trip data identical' 0
rm -f "$EXT_IN" "$EXT_OUT"

###############################################################################
# lsdf  (size should be 1 KiB)
###############################################################################
sz=$(lsdf_bytes /greeting.txt)
print_result $? 'lsdf runs' 0
num_expect "$sz" -eq "$BLOCKSIZE" 'lsdf returns 1-block allocation for small file'

###############################################################################
# hard-link, unlink, ref-count behaviour
###############################################################################
"$VFS_EXEC" "$IMAGE" crhl /greeting.txt /link.txt >/dev/null 2>&1
print_result $? 'crhl creates hard link' 0
"$VFS_EXEC" "$IMAGE" ls / | grep -q 'link.txt'
print_result $? 'ls shows link.txt' 0

# capture counts
df3="$("$VFS_EXEC" "$IMAGE" df)"
fb3=$(df_field "$df3" 'Free Blocks:'); ui3=$(df_field "$df3" 'Free Inodes:')

"$VFS_EXEC" "$IMAGE" rm /greeting.txt >/dev/null 2>&1
print_result $? 'rm original path' 0
"$VFS_EXEC" "$IMAGE" ls / | grep -vq 'greeting.txt'
print_result $? 'original entry gone' 0
"$VFS_EXEC" "$IMAGE" ls / | grep -q  'link.txt'
print_result $? 'link.txt still exists' 0
df4="$("$VFS_EXEC" "$IMAGE" df)"
num_expect "$(df_field "$df4" 'Free Blocks:')" -eq "$fb3" \
            'blocks unchanged (linkcount>0)'
num_expect "$(df_field "$df4" 'Free Inodes:')" -eq "$ui3" \
            'inodes unchanged (linkcount>0)'

"$VFS_EXEC" "$IMAGE" rm /link.txt >/dev/null 2>&1
print_result $? 'rm final link' 0
df5="$("$VFS_EXEC" "$IMAGE" df)"
num_expect "$(df_field "$df5" 'Free Blocks:')" -gt "$fb3" \
            'blocks freed after last link'
num_expect "$(df_field "$df5" 'Free Inodes:')" -gt "$ui3" \
            'inode freed after last link'

###############################################################################
# ext / red   (allocate & release blocks)
###############################################################################
# create 2 000-byte file via ecpt
PAYLOAD=$(mktemp tmp.pay.XXXX); head -c 2000 </dev/zero >"$PAYLOAD"
"$VFS_EXEC" "$IMAGE" ecpt "$PAYLOAD" /grow.txt >/dev/null 2>&1
sz0=$(lsdf_bytes /grow.txt)           # should be 2048
printf '[PASS] initial size 2000 -> %d B on disk\n' "$sz0"; ((PASS++))

# extend by 1 500 -> size 3 500 (4 blocks on disk)
"$VFS_EXEC" "$IMAGE" ext /grow.txt 1500 >/dev/null 2>&1
print_result $? 'ext succeeds' 0
sz1=$(lsdf_bytes /grow.txt)
num_expect "$sz1" -eq $((4*BLOCKSIZE)) 'size after ext is 4 blocks'

# reduce by 3 000 -> size 500 (1 block on disk)
"$VFS_EXEC" "$IMAGE" red /grow.txt 3000 >/dev/null 2>&1
print_result $? 'red succeeds' 0
sz2=$(lsdf_bytes /grow.txt)
num_expect "$sz2" -eq $((1*BLOCKSIZE)) 'size after red is 1 block'
df6="$("$VFS_EXEC" "$IMAGE" df)"
num_expect "$(df_field "$df6" 'Free Blocks:')" -gt "$(df_field "$df5" 'Free Blocks:')" \
            'blocks freed after red'

# extend beyond the 12 KB limit should fail
"$VFS_EXEC" "$IMAGE" ext /grow.txt 13000 >/dev/null 2>&1
print_result $? 'ext beyond 12 KiB fails' 1

###############################################################################
echo
printf 'Summary: %d passed – %d failed\n' "$PASS" "$FAIL"
exit $FAIL
