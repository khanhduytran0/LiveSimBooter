# Pass this file as one -c argument to the pinned Bash with LSB_FORK_SNAPSHOT_PROBE=early.
: "${DYLD_ROOT_PATH:?Missing guest runtime}"
export PATH="$DYLD_ROOT_PATH/usr/local/bin:$DYLD_ROOT_PATH/bin:$DYLD_ROOT_PATH/usr/bin"
message='quoted pipeline payload'
unset child_value

# Expansion of $message and execution of the printf builtin both occur in the copied interpreter.
printf '%s\n' "${child_value:=$message}" | bash --noprofile --norc -c '
    IFS= read -r line || exit 90
    [[ $line == "$1" ]] || exit 91
    if IFS= read -r extra; then exit 92; fi
    exit 23
' receiver "$message"
stages=("${PIPESTATUS[@]}")
printf 'pipeline statuses %s %s\n' "${stages[@]}"
[[ ${#stages[@]} == 2 && ${stages[0]} == 0 && ${stages[1]} == 23 ]] || exit 93
[[ ${child_value-unset} == unset ]] || exit 96

bash --noprofile --norc -c 'exit 37' &
first=$!
bash --noprofile --norc -c 'exit 19' &
second=$!
[[ $first -gt 0 && $second -gt 0 && $first != "$second" ]] || exit 94
wait "$second"
second_status=$?
wait "$first"
first_status=$?
printf 'parent %s waited for %s -> %s and %s -> %s\n' "$$" "$second" "$second_status" "$first" "$first_status"
[[ $second_status == 19 && $first_status == 37 ]] || exit 95
