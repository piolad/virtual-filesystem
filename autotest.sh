#!/bin/bash

VFS_EXEC="./vfs"
IMAGE="test.img"
DISK_SIZE=$((1024 * 1024))  # 1MB
TEST_PASS=0
TEST_FAIL=0

print_result() {
    local status=$1
    local message="$2"
    local expected_status=$3
    if [ "$status" -eq "$expected_status" ]; then
        echo "[PASS] $message"
        ((TEST_PASS++))
    else
        echo "[FAIL] $message"
        echo "       Expected exit code $expected_status but got $status"
        ((TEST_FAIL++))
    fi
}

cleanup() {
    rm -f "$IMAGE"
}

parse_df_value() {
    echo "$1" | grep "$2" | awk '{ print $NF }'
}

run_test() {
    cleanup

    $VFS_EXEC "$IMAGE" mkfs "$DISK_SIZE" > /dev/null 2>&1
    print_result $? "mkfs creates image" 0

    ######################
    # Test initial df
    ######################
    df_output=$($VFS_EXEC "$IMAGE" df 2>/dev/null)
    total_blocks=$(parse_df_value "$df_output" "Total Blocks:")
    free_blocks_before=$(parse_df_value "$df_output" "Free Blocks:")
    used_blocks_before=$(parse_df_value "$df_output" "Used Blocks:")
    free_inodes_before=$(parse_df_value "$df_output" "Free Inodes:")

    [[ "$total_blocks" -gt 0 && "$free_blocks_before" -gt 0 ]] && print_result 0 "df shows initial block stats" 0 || print_result 1 "df shows initial block stats" 0

    ######################
    # Create directories
    ######################
    $VFS_EXEC "$IMAGE" mkdir /dirA > /dev/null 2>&1
    print_result $? "mkdir /dirA" 0

    $VFS_EXEC "$IMAGE" mkdir /dirA/subdir > /dev/null 2>&1
    print_result $? "mkdir /dirA/subdir" 0

    ######################
    # Try rmdir on non-empty directory — should fail
    ######################
    $VFS_EXEC "$IMAGE" rmdir /dirA > /dev/null 2>&1
    print_result $? "rmdir /dirA (non-empty) fails as expected" 1

    ######################
    # Remove subdir first
    ######################
    $VFS_EXEC "$IMAGE" rmdir /dirA/subdir > /dev/null 2>&1
    print_result $? "rmdir /dirA/subdir (empty) succeeds" 0

    ######################
    # Remove parent now — should succeed
    ######################
    $VFS_EXEC "$IMAGE" rmdir /dirA > /dev/null 2>&1
    print_result $? "rmdir /dirA (now empty) succeeds" 0

    ######################
    # Try rmdir on nonexistent path
    ######################
    $VFS_EXEC "$IMAGE" rmdir /no-such-dir > /dev/null 2>&1
    print_result $? "rmdir /no-such-dir fails" 1

    ######################
    # Compare df stats after rmdir
    ######################
    df_output_after=$($VFS_EXEC "$IMAGE" df 2>/dev/null)
    free_blocks_after=$(parse_df_value "$df_output_after" "Free Blocks:")
    used_blocks_after=$(parse_df_value "$df_output_after" "Used Blocks:")
    free_inodes_after=$(parse_df_value "$df_output_after" "Free Inodes:")

    ((free_blocks_after >= free_blocks_before)) && print_result 0 "df free blocks increased after rmdir" 0 || print_result 1 "df free blocks increased after rmdir" 0
    ((free_inodes_after >= free_inodes_before)) && print_result 0 "df free inodes increased after rmdir" 0 || print_result 1 "df free inodes increased after rmdir" 0

    cleanup
}

echo "Running vfs automated tests..."
run_test
echo
echo "Tests Passed: $TEST_PASS"
echo "Tests Failed: $TEST_FAIL"

exit $TEST_FAIL
