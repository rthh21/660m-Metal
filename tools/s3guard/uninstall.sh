#!/bin/sh
# Stops org.rmb.s3guard and moves its two installed files to tools/s3guard/removed_<date> (nothing is
# deleted). Its logs stay in /private/var/log. Run from the repository root: sudo sh tools/s3guard/uninstall.sh
set -e
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
H=$(cd "$(dirname "$0")" && pwd)
K="$H/removed_$(date +%Y%m%d_%H%M%S)"
launchctl bootout system/org.rmb.s3guard 2>/dev/null || true
mkdir -p "$K"
[ -f /Library/LaunchDaemons/org.rmb.s3guard.plist ] && mv /Library/LaunchDaemons/org.rmb.s3guard.plist "$K/"
[ -f /usr/local/libexec/rmb-s3guard.sh ] && mv /usr/local/libexec/rmb-s3guard.sh "$K/"
echo "s3guard stopped; its files are in $K"
