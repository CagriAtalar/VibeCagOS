#!/bin/sh
# Deterministic QEMU serial tests for the Ring-3 / syscall / scheduler work.
# usage: tests/run.sh <ring3|syscall|scheduler|usercopy|faults>
# Boots os.iso headless, types shell commands over the serial port, captures
# the serial log and greps for expected lines. Exit code != 0 on any failure.
QEMU=${QEMU:-qemu-system-i386}
SUITE=$1
IMG=$(mktemp /tmp/vibe-disk.XXXXXX); LOG=$(mktemp /tmp/vibe-log.XXXXXX)
dd if=/dev/zero of="$IMG" bs=1M count=2 status=none
trap 'rm -f "$IMG" "$LOG"' EXIT

case $SUITE in
  ring3)     CMDS="utest 0|utest 12|ps" ;;
  syscall)   CMDS="utest 1|utest 2|utest 6" ;;
  scheduler) CMDS="utest2 4 5|uspawn 4|utest 6|ps" ;;
  usercopy)  CMDS="utest 7|ps" ;;
  faults)    CMDS="utest 3|utest 8|utest 9|utest 10|utest 11|utest 0|ps" ;;
  *) echo "unknown suite $SUITE"; exit 2 ;;
esac

# Drive the serial console through a FIFO; send each command only after the
# previous prompt appeared (no fixed sleeps -> not timing sensitive).
FIFO=$(mktemp -u /tmp/vibe-fifo.XXXXXX); mkfifo "$FIFO"
$QEMU -cdrom os.iso -drive file="$IMG",format=raw,if=ide \
      -no-reboot -m 128M -serial stdio -display none < "$FIFO" > "$LOG" 2>&1 &
QPID=$!
exec 3> "$FIFO"
prompts() { tr -d '\r' < "$LOG" | grep -ac 'vcos:/\$ '; }
wait_prompts() {   # $1 = wanted prompt count, 30 s limit
  i=0; while [ "$(prompts)" -lt "$1" ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done
}
wait_prompts 1
n=1
for c in $(echo "$CMDS" | tr ' ' '~' | tr '|' ' '); do
  printf '%s\n' "$(echo "$c" | tr '~' ' ')" >&3
  n=$((n+1)); wait_prompts $n
done
printf 'exit\n' >&3
i=0; while kill -0 $QPID 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
kill $QPID 2>/dev/null; exec 3>&-; rm -f "$FIFO"
tr -d '\r' < "$LOG" > "$LOG.clean"; mv "$LOG.clean" "$LOG"

FAIL=0
expect() { if grep -Eq -- "$1" "$LOG"; then echo "  PASS  $2"; else echo "  FAIL  $2   (/$1/)"; FAIL=1; fi; }
reject() { if grep -Eq -- "$1" "$LOG"; then echo "  FAIL  $2   (/$1/ present)"; FAIL=1; else echo "  PASS  $2"; fi; }

echo "== suite: $SUITE =="
case $SUITE in
 ring3)
  expect 'T0: hello from ring3'            'user code runs'
  expect 'T0: cs&3=3 ss&3=3 IF=1'          'CPL=3 for CS and SS, IF=1'
  expect 'pid [0-9]+ finished, exit code 0' 'process exits cleanly'
  expect 'finished, exit code 42'          'exit status propagated'
  reject 'zombie|ZOMBIE  utest'            'no zombie left after wait' ;;
 syscall)
  expect 'T1: getpid=1'                    'SYS_GETPID'
  expect 'T1: putchar=X'                   'SYS_PUTCHAR'
  expect 'T1: uptime_ok=1'                 'SYS_UPTIME'
  expect 'T1: invalid_syscall=-38'         'invalid syscall -> -ENOSYS'
  expect 'T1: write_badfd=-9'              'bad fd -> -EBADF'
  expect 'T2: data=1235 ro0=114'           'RW data page + RO text page readable'
  expect 'T6: slept_ok=1'                  'SYS_SLEEP ~300ms then wakeup' ;;
 scheduler)
  expect 'A+B+A|B+A+B'                     'A and B interleave without yield (preemption)'
  expect 'exit 10, pid [0-9]+ exit 11'     'both finish with own exit codes'
  expect '\[uspawn\] pid'                  'background process spawned'
  expect 'T6: slept_ok=1'                  'sleep works while others run' ;;
 usercopy)
  expect 'T7: write_null=-14'              'NULL'
  expect 'T7: write_kernel=-14'            'kernel address'
  expect 'T7: write_unmapped=-14'          'unmapped user address'
  expect 'T7: write_wrap=-14'              'address wrap-around'
  expect 'T7: cross_page_stack=-14'        'buffer crossing into unmapped page'
  expect 'T7: readonly_dst=-14'            'read-only destination page'
  expect 'T7: getinfo_ok=0 pid_ok=1'       'valid copy_to_user works'
  expect 'T7: done'                        'kernel survived'
  reject 'PANIC|EXCEPTION'                 'no kernel panic' ;;
 faults)
  expect 'T3: before kernel read'          'T3 started'
  expect 'user page fault at 0x00100000'   'user read of kernel memory -> page fault'
  expect 'T8: before cli'                  'T8 started'
  expect 'General Protection.*killed'      'cli / out / int 0x20 -> #GP kills process'
  reject 'SURVIVED'                        'no faulting instruction completed'
  expect 'user page fault at 0x10000008'   'write to RO text page -> fault'
  expect 'T0: hello from ring3'            'system still works after 5 killed processes'
  reject 'KERNEL PANIC|EXCEPTION'          'no kernel panic' ;;
esac
[ $FAIL -eq 0 ] && echo "  => OK" || { echo "  => FAILED (log kept in /tmp/vibe-last.log)"; cp "$LOG" /tmp/vibe-last.log; }
exit $FAIL
