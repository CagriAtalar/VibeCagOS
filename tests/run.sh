#!/bin/sh
# Deterministic QEMU serial tests for the Ring-3 / syscall / scheduler work.
# usage: tests/run.sh <ring3|syscall|scheduler|usercopy|faults|stdio|fs>
# Boots os.iso headless, types shell commands over the serial port, captures
# the serial log and greps for expected lines. Exit code != 0 on any failure.
QEMU=${QEMU:-qemu-system-i386}
SUITE=$1
IMG=$(mktemp /tmp/vibe-disk.XXXXXX); LOG=$(mktemp /tmp/vibe-log.XXXXXX)
dd if=/dev/zero of="$IMG" bs=1M count=2 status=none
trap '' PIPE   # QEMU may exit while we still write: never die of SIGPIPE
trap 'rm -f "$IMG" "$LOG"' EXIT

case $SUITE in
  ring3)     CMDS="utest 0|utest 12|ps" ;;
  syscall)   CMDS="utest 1|utest 2|utest 6" ;;
  scheduler) CMDS="utest2 4 5|uspawn 4|utest 6|ps" ;;
  usercopy)  CMDS="utest 7|ps" ;;
  stdio)     CMDS="utest 13|utest 14<abc|utest 16|utest 16|ps" ;;
  fs)        CMDS="utest 15|utest 15|utest 16|ps" ;;
  faults)    CMDS="utest 3|utest 8|utest 9|utest 10|utest 11|utest 0|ps" ;;
  *) echo "unknown suite $SUITE"; exit 2 ;;
esac

# Drive the serial console through a FIFO; send each command only after the
# previous prompt appeared (no fixed sleeps -> not timing sensitive).
FIFO=$(mktemp -u /tmp/vibe-fifo.XXXXXX); mkfifo "$FIFO"
$QEMU $QEMU_EXTRA -cdrom os.iso -drive file="$IMG",format=raw,if=ide \
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
  line=$(echo "$c" | tr '~' ' ')
  case $line in
    *"<"*) printf '%s\n' "${line%%<*}" >&3
           w=0; while ! tr -d '\r' < "$LOG" | grep -aq 'T14: waiting' && [ $w -lt 100 ]; do sleep 0.1; w=$((w+1)); done
           sleep 0.3; printf '%s\n' "${line#*<}" >&3 ;;
    *) printf '%s\n' "$line" >&3 ;;
  esac
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
 stdio)
  expect 'T13: stdout=ok2'                 'write(1) works'
  expect 'T13: stderr=ok2'                 'write(2) works'
  expect 'T13: write_stdin=-9'             'write to stdin -> EBADF'
  expect 'T13: read_stdout=-9'             'read from stdout -> EBADF'
  expect 'T13: fstat0=0 type=4'            'fd 0 is a character device'
  expect 'T13: close_bad=-9'               'close(99) -> EBADF'
  expect 'T14: read=4 data=abc'            'blocking read(0) got serial input'
  expect 'T16: opened=13 then=-24'         'fd table limit -> EMFILE'
  expect 'T16: reopen=3'                   'closed fds are reused (lowest free)'
  reject 'PANIC|EXCEPTION|killed'          'no panic / no kill' ;;
 fs)
  expect 'T15: open_create=3'              'first fd is 3'
  expect 'T15: write=8'                    'write to file'
  expect 'T15: read_on_wronly=-9'          'EBADF for read on O_WRONLY'
  expect 'T15: close=0'                    'close'
  expect 'T15: close_again=-9'             'double close -> EBADF'
  expect 'T15: read=8 data=hello fs'       'read back data'
  expect 'T15: write_on_rdonly=-9'         'write on O_RDONLY fd -> EBADF (POSIX)'
  expect 'T15: lseek=6 tail=fs'            'lseek + read'
  expect 'T15: fstat=0 size=8 type=1'      'fstat'
  expect 'T15: stat=0 size=8'              'stat'
  expect 'T15: open_missing=-2'            'ENOENT'
  expect 'T15: stat_missing=-2'            'stat ENOENT'
  expect 'T15: open_badptr=-14'            'bad path pointer -> EFAULT'
  expect 'T15: stat_badbuf=-14'            'bad stat buffer -> EFAULT'
  expect 'T15: after_append_size=10'       'O_APPEND'
  expect 'T15: after_trunc_size=0'         'O_TRUNC'
  expect 'T15: mkdir=0'                    'mkdir'
  expect 'T15: mkdir_again=-17'            'mkdir existing -> EEXIST'
  expect 'T15: dir_type=2'                 'stat dir'
  expect 'T15: readdir_found_d15=1'        'readdir lists new directory'
  expect 'T15: write_dir=-21'              'open dir for write -> EISDIR'
  expect 'T15: rmdir=0'                    'rmdir'
  expect 'T15: unlink=0'                   'unlink'
  expect 'T15: stat_after_unlink=-2'       'gone after unlink'
  reject 'PANIC|EXCEPTION|killed'          'no panic / no kill' ;;
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
