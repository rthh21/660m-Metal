#!/bin/sh
# org.rmb.s3guard: safety net for bare-metal S3 sleep with Rembrandt660M 1.1.0 or newer.
# Runs as root from /Library/LaunchDaemons/org.rmb.s3guard.plist (installed copy:
# /usr/local/libexec/rmb-s3guard.sh, root-owned).
#
#  1. At boot: if the previous boot ended in the middle of a sleep or wake (the kext's last "s3guard:"
#     line is not a finished wake: hang, panic or forced power-off), turn sleep off (pmset disablesleep 1)
#     and write why to /private/var/log/rmb-s3guard.state. No restart (we are already freshly booted).
#  2. Live: the kext line "s3guard: WAKE FAILED", or AMD's "TTL::powerUp() Failed" within 15 min of
#     a sleep: the GPU did not come back (11-12: black screens). Turn sleep off, write why, sync,
#     clean restart; forced restart if the clean one has not happened 60 s later.
#
# Every s3guard/TTL line is copied to /private/var/log/rmb-s3guard.log (the kernel logs rotate fast).
# Turn sleep back on after a trip, once the cause is understood: sudo pmset -a disablesleep 0

PATH=/usr/bin:/bin:/usr/sbin:/sbin
MSG=/private/var/log/rmb-msgbuf.log
PREV=/private/var/log/rmb-msgbuf.prev.log
STATE=/private/var/log/rmb-s3guard.state
LOG=/private/var/log/rmb-s3guard.log
RUN=/private/var/run/rmb-s3guard   # cleared at every boot
WINDOW=900                          # s after a sleep entry / wake start in which a TTL failure counts

mkdir -p $RUN
say() { echo "$(date '+%F %T') $*" >> $LOG; }

trip() {  # $1 restart|norestart, $2 reason
	say "TRIP ($1): $2"
	pmset -a disablesleep 1 >> $LOG 2>&1
	echo "$(date '+%F %T') sleep disabled by s3guard: $2" >> $STATE
	sync
	[ "$1" = restart ] || return 0
	mkdir $RUN/restarting 2>/dev/null || return 0  # one restart per boot
	say "restarting cleanly (GPU did not come back from sleep)"
	sync
	shutdown -r now "s3guard: GPU did not come back from sleep" >> $LOG 2>&1
	sleep 60
	say "clean restart did not happen in 60 s, forcing"
	sync
	reboot -q
}

# --- 1. previous boot ---------------------------------------------------------------------------
bootscan() {
	bt=$(sysctl -n kern.boottime | sed -E 's/^\{ sec = ([0-9]+),.*/\1/')
	[ "$(cat $RUN/bootscan 2>/dev/null)" = "$bt" ] && return 0  # daemon restarted, same boot
	# The kext opens its log ~5-30 s into the boot; if the old log was big it first copies its end
	# to PREV, else it appends to MSG after a "new boot" separator.
	i=0
	while [ $i -lt 90 ]; do
		[ -f $MSG ] && [ "$(stat -f %m $MSG)" -ge "$bt" ] && break
		i=$((i + 1)); sleep 2
	done
	echo "$bt" > $RUN/bootscan
	if [ -f $PREV ] && [ "$(stat -f %m $PREV)" -ge "$bt" ]; then
		src=$PREV
		lines=$(grep -a 'Rembrandt660M: s3guard: ' $PREV)
	elif [ -f $MSG ] && [ "$(stat -f %m $MSG)" -ge "$bt" ]; then
		src="$MSG (previous section)"
		c=$(grep -a -c 'disklog: new boot' $MSG)
		lines=$(awk -v c="$c" '/disklog: new boot/ {n++} n == c - 1' $MSG | grep -a 'Rembrandt660M: s3guard: ')
	else
		say "boot: kernel log not written this boot after 180 s, previous boot not checked"
		return 0
	fi
	[ -n "$lines" ] || { say "boot: previous boot has no s3guard lines in $src (no sleep in its last minutes)"; return 0; }
	# Walk the previous boot's s3guard lines: a sleep is finished by "wake OK", or by a full wake
	# with the GPU never powered down (aborted sleep). Anything else last = it died inside S3.
	st=$(printf '%s\n' "$lines" | awk '
		/s3guard: sleep entry/               { s = "asleep" }
		/s3guard: GPU down for sleep/        { s = "asleep (GPU down)" }
		/s3guard: GPU power-up after sleep/  { s = "waking (GPU power-up started)" }
		/s3guard: WAKE FAILED/               { s = "wake FAILED" }
		/s3guard: wake OK/                   { s = "ok" }
		/s3guard: full wake \(GPU not down/  { s = "ok" }
		END { print s }')
	last=$(printf '%s\n' "$lines" | tail -n 1 | sed -E 's/.*Rembrandt660M: //')
	if [ "$st" = ok ]; then
		say "boot: previous boot's last sleep finished fine ($last)"
	else
		trip norestart "previous boot ended while $st; last kext line: $last"
	fi
}

# --- 2. live ------------------------------------------------------------------------------------
recent_sleep() {
	now=$(date +%s)
	# kern.waketime: set by the kernel at every (dark) wake, 0 before the first one. The kext's
	# "GPU power-up after sleep" line can reach the file after AMD's failure line, so check both.
	wt=$(sysctl -n kern.waketime | sed -E 's/^\{ sec = ([0-9]+),.*/\1/')
	[ "${wt:-0}" -gt 0 ] && [ $((now - wt)) -lt $WINDOW ] && return 0
	[ -f $RUN/armed ] && [ $((now - $(stat -f %m $RUN/armed))) -lt $WINDOW ]
}

watch_kext() {
	tail -n 0 -F $MSG 2>/dev/null | grep --line-buffered -a 'Rembrandt660M: s3guard: ' | while IFS= read -r l; do
		l=${l#*Rembrandt660M: }
		say "kext: $l"
		case "$l" in
		*"WAKE FAILED"*) trip restart "$l" ;;
		*"sleep entry"* | *"GPU power-up after sleep"*) touch $RUN/armed ;;
		esac
	done
}

watch_amd() {
	log stream --style compact --level default \
		--predicate 'process == "kernel" AND (eventMessage CONTAINS "TTL::powerUp" OR eventMessage CONTAINS "TTL:powerUp" OR eventMessage CONTAINS "TTL::powerDown")' \
		2>/dev/null | while IFS= read -r l; do
		case "$l" in
		*"kernel["*TTL*) ;;  # not the "Filtering the log data" header
		*) continue ;;
		esac
		say "amd: ${l#*kernel\[*\] }"
		case "$l" in
		*"TTL::powerUp() Failed"*)
			if recent_sleep; then trip restart "AMD TTL::powerUp failed after sleep"
			else say "TTL::powerUp failure not near a sleep: no action"; fi ;;
		esac
	done
}

say "start (pid $$)"
bootscan
watch_kext &
k=$!
watch_amd &
a=$!
# Either watcher gone (log stream or tail died) -> exit, launchd starts us again (KeepAlive).
while kill -0 $k 2>/dev/null && kill -0 $a 2>/dev/null; do sleep 20; done
say "a watcher exited, restarting"
pkill -P $$ 2>/dev/null
exit 1
