#!/bin/sh
# Run every i386 QEMU regression suite (each on its own disposable disk image)
# and print one PASS/FAIL line per suite plus a summary. Exit != 0 on failure.
# usage: tests/all.sh [-v]     (-v prints every check, not just failures)
cd "$(dirname "$0")/.." || exit 2
VERBOSE=0; [ "$1" = "-v" ] && VERBOSE=1
SUITES="ring3 syscall scheduler usercopy faults stdio fs shell pipe proc exec"
pass=0; fail=0; failed=""
for s in $SUITES persist; do
  if [ "$s" = persist ]; then out=$(sh tests/persist.sh 2>&1); else out=$(sh tests/run.sh "$s" 2>&1); fi
  rc=$?
  [ $VERBOSE = 1 ] && echo "$out" || echo "$out" | grep -E 'FAIL|suite'
  if [ $rc -eq 0 ]; then echo "SUITE $s: PASS"; pass=$((pass+1))
  else echo "SUITE $s: FAIL"; fail=$((fail+1)); failed="$failed $s"; fi
done
echo "== $pass passed, $fail failed${failed:+ (failed:$failed)} =="
[ $fail -eq 0 ]
