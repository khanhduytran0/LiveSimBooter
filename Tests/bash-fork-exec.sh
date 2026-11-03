# Pass this file's contents to the pinned guest Bash as one -c argument. See docs/bash.md.
: "${DYLD_ROOT_PATH:?Missing guest runtime}"
export PATH="$DYLD_ROOT_PATH/usr/local/bin:$DYLD_ROOT_PATH/bin:$DYLD_ROOT_PATH/usr/bin"
export LSB_FORK_TEST_VALUE='spaces in environment'
printf 'outer Bash %s\n' "$$"

bash --noprofile --norc -c '
    [[ $0 == "child shell" && $# == 2 && $1 == "quoted word" && -z $2 ]] || exit 91
    [[ $LSB_FORK_TEST_VALUE == "spaces in environment" ]] || exit 92
    df -P . || exit 93
    printf "inner Bash %s waited for df\n" "$$"
    exit 37
' 'child shell' 'quoted word' ''
rc=$?
printf 'outer Bash %s waited for inner Bash, status %d\n' "$$" "$rc"
[[ $rc == 37 ]] || exit 94

df -P /no-lsb-fork-test-filesystem 2>&1
rc=$?
printf 'outer Bash %s waited for failing df, status %d\n' "$$" "$rc"
[[ $rc == 1 ]] || exit 95
