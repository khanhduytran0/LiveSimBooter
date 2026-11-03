# Pass this script as one -c argument with LSB_FORK_SNAPSHOT_PROBE=early on the pinned guest Bash.
: "${DYLD_ROOT_PATH:?Missing guest runtime}"
export PATH="$DYLD_ROOT_PATH/bin:$DYLD_ROOT_PATH/usr/bin:$DYLD_ROOT_PATH/usr/local/bin"

(exit 37)
status=$?
printf 'parent %s observed subshell exit %d\n' "$$" "$status"
[[ $status == 37 ]] || exit 91

out=$(df -P .)
status=$?
printf 'assignment status %d, captured table follows\n%s\n' "$status" "$out"
[[ $status == 0 && $out == Filesystem* && $out == *$'\n'* && $out != *$'\n' ]] || exit 92

out=$(printf 'first line\nsecond line\n\n'; exit 41)
status=$?
[[ $status == 41 && $out == $'first line\nsecond line' ]] || exit 93
printf 'multiline capture preserved, assignment status %d\n' "$status"

out=$(df -P /no-lsb-fork-test-filesystem)
status=$?
[[ $status == 1 && -z $out ]] || exit 94
printf 'failing df status %d, captured stdout empty\n' "$status"
