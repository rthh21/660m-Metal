#!/bin/sh
# Installs the S3 safety net (org.rmb.s3guard, see s3guard.sh). Run from the repository root: sudo sh tools/s3guard/install.sh
# Harmless while sleep is still disabled: it only checks the previous boot and watches the logs.
# Undo: sudo sh tools/s3guard/uninstall.sh
set -e
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
H=$(cd "$(dirname "$0")" && pwd)
plutil -lint "$H/org.rmb.s3guard.plist"
sh -n "$H/s3guard.sh"
mkdir -p /usr/local/libexec
install -o root -g wheel -m 755 "$H/s3guard.sh" /usr/local/libexec/rmb-s3guard.sh
install -o root -g wheel -m 644 "$H/org.rmb.s3guard.plist" /Library/LaunchDaemons/org.rmb.s3guard.plist
launchctl bootout system/org.rmb.s3guard 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/org.rmb.s3guard.plist
sleep 4
launchctl print system/org.rmb.s3guard | grep -E '^\s+(state|pid) ='
echo "--- /private/var/log/rmb-s3guard.log:"
tail -n 4 /private/var/log/rmb-s3guard.log
echo "s3guard installed (script $(shasum -a 256 /usr/local/libexec/rmb-s3guard.sh | cut -c1-8))"
