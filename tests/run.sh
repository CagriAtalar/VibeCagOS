#!/bin/sh
# Deterministic QEMU serial tests for the Ring-3 / syscall / scheduler work.
# usage: tests/run.sh <ring3|syscall|scheduler|usercopy|faults|stdio|fs|shell|pipe|proc|exec>
# Boots os.iso headless, types shell commands over the serial port, captures
# the serial log and greps for expected lines. Exit code != 0 on any failure.
QEMU=${QEMU:-qemu-system-i386}
SUITE=$1
IMG=$(mktemp /tmp/vibe-disk.XXXXXX); LOG=$(mktemp /tmp/vibe-log.XXXXXX)
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
trap '' PIPE   # QEMU may exit while we still write: never die of SIGPIPE
trap 'rm -f "$IMG" "$LOG"' EXIT

# Commands are separated by '@' (not '|', which the shell itself uses for
# pipelines). Spaces become '~' so word splitting does not split a command.
case $SUITE in
  ring3)     CMDS="utest 0@utest 12@ps" ;;
  syscall)   CMDS="utest 1@utest 2@utest 6" ;;
  scheduler) CMDS="utest 4 &@utest 5 &@wait@utest 4 &@utest 6@wait@ps" ;;
  usercopy)  CMDS="utest 7@ps" ;;
  stdio)     CMDS="utest 13@utest 14<abc@utest 16@utest 16@ps" ;;
  shell)     CMDS="pwd@mkdir /a@cd /a@pwd@echo hi there > f.txt@cat f.txt@cp f.txt g.txt@ls@mv g.txt h.txt@stat h.txt@head h.txt@cd ..@rm /a/f.txt@rm /a/h.txt@rmdir /a@ls@uname@ps@cat /proc/meminfo@nosuch@kill 9999@ls /nonexistent@cd /nonexistent@write w.txt hello world@cat w.txt@echo more >> w.txt@cat w.txt@hexdump w.txt@rm w.txt@utest 19@pwd@ls /bin@echo piped | cat@date@devices" ;;
  # Pids: 0 idle, 1 kinit (kernel), 2 init (Ring 3), 3 sh. ps/kill/sleep are
  # ordinary /bin programs now, so they consume pids too; the sequence below is
  # fixed, so the ids stay deterministic. utest 17 & = pid 4; ps = 5; kill = 6;
  # ps = 7; utest 18 = 8 (children 9,10); utest 20 = 11, its orphaned sleeper = 12.
  proc)      CMDS="free@utest 17 &@ps@kill 4@ps@wait@utest 18@utest 20@ps@kill 12@sleep 300@ps@free" ;;
  fs)        CMDS="utest 15@utest 28@utest 15@utest 16@ps" ;;
  faults)    CMDS="utest 3@utest 8@utest 9@utest 10@utest 11@utest 0@ps" ;;
  pipe)      CMDS="utest 21@utest 25 | utest 26@utest 25 | cat@ls /bin | cat@utest 1 > /redir.txt@cat /redir.txt@ps" ;;
  exec)      CMDS="ls /bin@ls -l /bin@utest 24@utest 27@utest 22@ps" ;;
  *) echo "unknown suite $SUITE"; exit 2 ;;
esac

# Drive the serial console through a FIFO; send each command only after the
# previous prompt appeared (no fixed sleeps -> not timing sensitive).
FIFO=$(mktemp -u /tmp/vibe-fifo.XXXXXX); mkfifo "$FIFO"
$QEMU $QEMU_EXTRA -cdrom os.iso -drive file="$IMG",format=raw,if=ide \
      -no-reboot -m 128M -serial stdio -display none < "$FIFO" > "$LOG" 2>&1 &
QPID=$!
exec 3> "$FIFO"
prompts() { tr -d '\r' < "$LOG" | grep -aEc 'vcos:[^ ]*\$ '; }
wait_prompts() {   # $1 = wanted prompt count, 30 s limit
  i=0; while [ "$(prompts)" -lt "$1" ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done
}
wait_prompts 1
n=1
for c in $(echo "$CMDS" | tr ' ' '~' | tr '@' ' '); do
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
  expect '\[pid [0-9]+\] exit code 0' 'process exits cleanly'
  expect '\[pid [0-9]+\] exit code 42'  'exit status propagated'
  reject 'zombie|ZOMBIE  utest'            'no zombie left after wait' ;;
 syscall)
  expect 'T1: getpid=1'                    'SYS_GETPID'
  expect 'T1: putchar=X'                   'SYS_PUTCHAR'
  expect 'T1: uptime_ok=1'                 'SYS_UPTIME'
  expect 'T1: invalid_syscall=-38'         'invalid syscall -> -ENOSYS'
  expect 'T1: write_badfd=-9'              'bad fd -> -EBADF'
  expect 'T2: data=1235 ro0=114 bss0=0'    'RW data + RO text readable, BSS zeroed by loader'
  expect 'T6: slept_ok=1'                  'SYS_SLEEP ~300ms then wakeup' ;;
 scheduler)
  expect 'A+B+A|B+A+B'                     'A and B interleave without yield (preemption)'
  expect '\[pid [0-9]+\] exit code 10'     'A exits with its own code'
  expect '\[pid [0-9]+\] exit code 11'     'B exits with its own code'
  expect '\[pid [0-9]+\] started'          'background process spawned (shell stays interactive)'
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
 shell)
  expect '^/a$'                            'cd updates per-process cwd; pwd'
  expect '^hi there$'                      'echo > file ; cat'
  expect '^-  f.txt'                       'ls shows f.txt'
  expect '^-  g.txt'                       'cp created g.txt (mv checked by stat h.txt)'
  expect 'Size: 9'                         'stat'
  reject '^d  a$'                          'rm + rmdir removed /a'
  expect 'VibeCagOS 0.5.0'                 'uname (procfs)'
  expect 'RUNNING   ps'                    'ps runs as its own process in /bin'
  expect 'BLOCKED   sh'                    'ps sees the shell blocked in waitpid'
  expect '^-  pwd$'                        'pwd is an installed /bin program'
  expect '^-  mkdir$'                      'mkdir is an installed /bin program'
  expect '^piped$'                         'echo | cat: two /bin programs in a pipeline'
  expect 'MemTotal|Total|Free'             'meminfo (procfs)'
  expect 'sh: nosuch: command not found'   'unknown command'
  expect 'kill: 9999: No such process'     'kill missing pid -> ESRCH'
  expect 'ls: /nonexistent: No such file'  'ls error'
  expect 'cd: /nonexistent: No such file'  'cd error'
  expect '^hello world$'                   'write + cat'
  expect '^more$'                          'echo >> appends'
  expect '68 65 6c 6c 6f'                  'hexdump'
  expect 'T19: chdir=0'                    'child chdir works'
  expect 'T19: cwd=/proc'                  'child sees its own cwd'
  expect 'T19: relative_open_ok=1'         'relative path resolved against child cwd'
  expect 'T19: chdir_file=-20'             'chdir to a file -> ENOTDIR'
  expect 'T19: dotdot=0 /'                 '.. normalisation'
  expect 'vcos:/\$ pwd'                    'parent cwd unchanged by child chdir'
  expect 'UTC'                             'date'
  expect 'Detected Devices'                'devices'
  reject 'PANIC|EXCEPTION|killed'          'no panic / no kill' ;;
 proc)
  expect '\[pid 4\] started'               'background spawn gets pid 4 (0 idle, 1 kinit, 2 init, 3 sh)'
  expect 'SLEEPING  utest'                 'ps shows sleeping child'
  expect 'kill: terminated pid 4'          'kill'
  expect 'ZOMBIE    utest'                 'killed child is a zombie until reaped'
  expect '\[pid 4\] exit code -9'          'wait reaps it with status -9'
  expect 'T18: spawned=1'                  'Ring 3 spawn x2'
  expect 'T18: wait1=1 st1=42 wait2=1 st2=0' 'waitpid(pid) and waitpid(-1) return child status'
  expect 'T18: no_more_children=-10'      'waitpid with no children -> ECHILD'
  expect 'T18: spawn_missing=-2'           'spawn unknown program -> ENOENT'
  expect 'T18: spawn_badptr=-14'           'spawn bad pointer -> EFAULT'
  expect 'T18: wait_badptr=-14'            'waitpid bad pointer -> EFAULT'
  expect '\[pid [0-9]+\] exit code 7'      'parent exit code'
  expect 'T20: child_spawned=1'            'orphan scenario: parent exits first'
  expect 'kill: terminated pid 12'         'orphaned child (ppid -1) can be killed by a /bin program'
  nframes=$(grep -a '^FramesFree' "$LOG" | awk '{print $2}' | sort -u | wc -l)
  if [ "$nframes" = 1 ]; then echo "  PASS  no frame leak (FramesFree identical before/after)"; else echo "  FAIL  frame leak: $(grep -a '^FramesFree' "$LOG" | tr '\n' ' ')"; FAIL=1; fi
  reject 'PANIC|EXCEPTION'                 'no panic' ;;
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
  expect 'T28: wrote=1572864 size=1572864 write_ok=1' 'large file: 1.5 MiB written past both indirect levels'
  expect 'T28: read_ok=1 size_ok=1'        'large file reads back byte-for-byte'
  expect 'T28: overwrite_ok=1 size_kept=1' 'in-place overwrite deep in double-indirect region'
  expect 'T28: trunc=0 trunc_size=204800 prefix_ok=1 eof_ok=1' 'truncate frees indirect blocks, prefix intact'
  expect 'T28: unlink_ok=1'                'large file removed'
  expect 'T28: pass=1'                     'large-file test passed'
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
  pipe)
  expect 'T21: pipe=0'                     'SYS_PIPE succeeds'
  expect 'T21: fds=3,4'                    'pipe ends get the two lowest free fds'
  expect 'T21: write=10'                   'write into the pipe'
  expect 'T21: read=10 data=pipe hello'    'read the bytes back in order'
  expect 'T21: eof_after_close=1'          'closing the write end yields EOF, not a hang'
  expect 'T21: full_write=1 w=4096'        'writing exactly the pipe capacity does not block'
  expect 'T21: drain_ok=1 got=4096'        'all buffered bytes survive the writer closing'
  expect 'T21: eof=1'                      'drained pipe then reports EOF'
  expect 'T21: badptr=-14'                 'pipe() with a kernel pointer -> EFAULT'
  expect 'T25: sent'                      'pipeline writer started'
  expect 'T26: got=21'                    'pipeline: 21 bytes crossed the pipe, then EOF'
  expect '^alpha$'                        'pipeline payload arrived intact'
  expect '^\-  utest'                     'ls /bin | cat: two separate /bin programs, one pipe'
  expect 'T1: getpid=1'                   'stdout redirected into /redir.txt (cat reads it back)'
  reject 'KERNEL PANIC|EXCEPTION'          'no kernel panic' ;;
  exec)
  expect '^-  utest'                        'VBIN programs installed in /bin'
  expect '^- 755 +[0-9]+ +utest'            'ls -l on a separate program: mode and size come from stat()'
  expect 'T24: missing=-2'                 'exec of a missing file -> ENOENT'
  expect 'T24: dir=-21'                    'exec of a directory -> EISDIR'
  expect 'T24: notelf=-8'                  'exec of a non-VBIN file -> ENOEXEC'
  expect 'T24: badptr=-14'                 'exec with a kernel pointer -> EFAULT'
  expect 'T24: alive'                      'process survives every rejected exec'
  expect 'T27: magic=-8'                   'bad VBIN magic rejected'
  expect 'T27: trunc=-8'                   'truncated VBIN header rejected'
  expect 'T27: version=-8'                 'bad VBIN version rejected'
  expect 'T27: flags=-8'                   'bad VBIN flags rejected'
  expect 'T27: memhuge=-8'                 'oversized VBIN image rejected'
  expect 'T27: entrykern=-8'               'VBIN entry below USER_BASE rejected'
  expect 'T27: entrydata=-8'               'VBIN entry outside text rejected'
  expect 'T27: bad_rejected=1'             'all 12 malformed images rejected'
  expect 'T27: alive'                      'process intact after 12 bad execs'
  expect 'T22: open_fd=3'                  'fd 3 allocated before exec'
  expect 'T23: argc=3 argv0=/bin/utest argv1=23 argv2=extra' 'exec passes argv'
  expect 'T23: leaked_fd=-9'               'exec closed everything but fd 0/1/2'
  expect 'T23: hello from the exec.d image' 'new image runs in the same pid'
  expect '\[pid [0-9]+\] exit code 55'      "exec'd image's exit status reaches the parent"
  reject 'SURVIVED'                        'no exec() returned after succeeding'
  reject 'KERNEL PANIC|EXCEPTION'          'no kernel panic' ;;
esac
[ $FAIL -eq 0 ] && echo "  => OK" || { echo "  => FAILED (log kept in /tmp/vibe-last.log)"; cp "$LOG" /tmp/vibe-last.log; }
exit $FAIL
