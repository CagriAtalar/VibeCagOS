#!/bin/sh
# Persistence test: boot twice on the SAME disk image and prove that a file
# written during the first boot is still there after a clean reboot.
#
# This is what makes VibeFS a filesystem rather than a RAM disk, and it guards
# the inode/superblock flush path (and the v2 block-map rebuild at mount).
# usage: tests/persist.sh
QEMU=${QEMU:-qemu-system-i386}
IMG=$(mktemp /tmp/vibe-persist.XXXXXX)
LOG=$(mktemp /tmp/vibe-persist-log.XXXXXX)
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
trap '' PIPE
trap 'rm -f "$IMG" "$LOG"' EXIT

prompts() { tr -d '\r' < "$LOG" | grep -aEc 'vcos:[^ ]*\$ '; }
wait_prompts() {   # $1 = wanted prompt count, 30 s limit
  i=0; while [ "$(prompts)" -lt "$1" ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done
}

# Boot on $IMG, type the '@'-separated commands $1, then shut down cleanly.
boot_send() {
  FIFO=$(mktemp -u /tmp/vibe-pfifo.XXXXXX); mkfifo "$FIFO"
  $QEMU $QEMU_EXTRA -cdrom os.iso -drive file="$IMG",format=raw,if=ide \
        -no-reboot -m 128M -serial stdio -display none < "$FIFO" > "$LOG" 2>&1 &
  QPID=$!
  exec 3> "$FIFO"
  wait_prompts 1
  n=1
  for c in $(echo "$1" | tr ' ' '~' | tr '@' ' '); do
    line=$(echo "$c" | tr '~' ' ')
    printf '%s\n' "$line" >&3
    n=$((n+1)); wait_prompts $n
  done
  printf 'exit\n' >&3
  i=0; while kill -0 $QPID 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
  kill $QPID 2>/dev/null; exec 3>&-; rm -f "$FIFO"
  tr -d '\r' < "$LOG" > "$LOG.clean"; mv "$LOG.clean" "$LOG"
}

FAIL=0
expect() { if grep -Eq -- "$1" "$LOG"; then echo "  PASS  $2"; else echo "  FAIL  $2   (/$1/)"; FAIL=1; fi; }
reject() { if grep -Eq -- "$1" "$LOG"; then echo "  FAIL  $2   (/$1/ present)"; FAIL=1; else echo "  PASS  $2"; fi; }

echo "== suite: persist =="

# --- boot 1: create the file on disk -------------------------------------
boot_send "write /persist.txt persistent-payload@cat /persist.txt@stat /persist.txt"
echo "-- boot 1 --"
expect 'persistent-payload'                  'file written during first boot'
reject 'PANIC|EXCEPTION'                     'boot 1 clean'

# --- boot 2: same disk, fresh kernel; the file must still be there --------
boot_send "ls@cat /persist.txt@stat /persist.txt"
echo "-- boot 2 (reboot on the same image) --"
reject 'reformatting'                        'existing image mounted, not reformatted'
expect 'persistent-payload'                  'file survived the reboot'
expect 'persist\.txt'                        'file listed after reboot'
reject 'PANIC|EXCEPTION'                     'boot 2 clean'

if [ $FAIL = 0 ]; then
  echo "  => OK"
else
  echo "  => FAILED (log kept in /tmp/vibe-persist-last.log)"
  cp "$LOG" /tmp/vibe-persist-last.log
fi
exit $FAIL
