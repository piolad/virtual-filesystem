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

run_test() {
    cleanup

    # Create filesystem image
    $VFS_EXEC "$IMAGE" mkfs "$DISK_SIZE" > /dev/null 2>&1
    print_result $? "mkfs creates image" 0

    # Create /home
    $VFS_EXEC "$IMAGE" mkdir /home > /dev/null 2>&1
    print_result $? "mkdir /home" 0

    # Create /home/user
    $VFS_EXEC "$IMAGE" mkdir /home/user > /dev/null 2>&1
    print_result $? "mkdir /home/user" 0

    # Create /home/user/docs
    $VFS_EXEC "$IMAGE" mkdir /home/user/docs > /dev/null 2>&1
    print_result $? "mkdir /home/user/docs" 0

    # Try to create /home/user/docs again — should fail
    $VFS_EXEC "$IMAGE" mkdir /home/user/docs > /dev/null 2>&1
    print_result $? "mkdir /home/user/docs (existing) should fail" 1

    # List root — should show 'home'
    output=$($VFS_EXEC "$IMAGE" ls / 2>/dev/null)
    echo "$output" | grep -q "home"
    print_result $? "ls / shows 'home'" 0

    # List /home — should show 'user'
    output=$($VFS_EXEC "$IMAGE" ls /home 2>/dev/null)
    echo "$output" | grep -q "user"
    print_result $? "ls /home shows 'user'" 0

    # List /home/user — should show 'docs'
    output=$($VFS_EXEC "$IMAGE" ls /home/user 2>/dev/null)
    echo "$output" | grep -q "docs"
    print_result $? "ls /home/user shows 'docs'" 0

    # Try to mkdir on a file path (invalid, once files are supported)
    # For now test mkdir on invalid path
    $VFS_EXEC "$IMAGE" mkdir /does/not/exist > /dev/null 2>&1
    print_result $? "mkdir /does/not/exist should fail" 1

    # Try to ls an invalid path
    $VFS_EXEC "$IMAGE" ls /nope > /dev/null 2>&1
    print_result $? "ls /nope should fail" 1

    cleanup
}

echo "Running vfs automated tests..."
run_test
echo
echo "Tests Passed: $TEST_PASS"
echo "Tests Failed: $TEST_FAIL"

exit $TEST_FAIL
