#!/bin/bash
# Rembrandt660M - salveaza logurile de pe macOS pe Desktop, verifica reteaua si Metal.
# Ruleaza in Terminal pe macOS (scrie "bash ", trage fisierul in Terminal, Enter):
#   bash RMB_CHECK.command        -> tot: log kext, ioreg, Wi-Fi (doar Wi-Fi, fara iPhone USB), Metal
#   bash RMB_CHECK.command net    -> doar reteaua, pe interfata implicita (de ex. iPhone USB, pentru comparatie)
#   bash RMB_CHECK.command lung   -> Wi-Fi 10 minute: ping continuu + viteza la 2 minute (prinde deconectarile)
# Nu instaleaza nimic, nu cere sudo, nu modifica sistemul; scrie in Desktop/out_<data>_<ora>
# si, daca incape, o arhiva out_*.tgz langa script (pe stick).
# Fisierele mari sunt comprimate (.gz) si, peste 240 KB, copiate si in bucati *.part-aa, -ab, ...:
# in out_20261110_014542 exact fisierele mari au ajuns pe Windows cu 0 octeti.
# Site-urile din site_uri.txt (langa script, un site pe linie) sunt testate in plus.
HERE="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-all}"
STAMP="$(date +%Y%m%d_%H%M%S)"
case "$MODE" in all) SUF="" ;; net) SUF="_net" ;; lung) SUF="_lung" ;; *) echo "folosire: bash RMB_CHECK.command [net|lung]"; exit 2 ;; esac
# Output always on the Desktop (internal disk, plenty of space). Terminal needs the
# "Desktop Folder" permission; without it fall back to the home folder.
BASE="$HOME/Desktop"
OUT="$BASE/out_${STAMP}${SUF}"
if ! { mkdir -p "$OUT" 2>/dev/null && : > "$OUT/system.txt" 2>/dev/null; }; then
	echo "ATENTIE: Terminal nu are voie sa scrie pe Desktop."
	echo "  (Setari Sistem > Confidentialitate si securitate > Fisiere si dosare > Terminal > Dosarul Desktop)"
	BASE="$HOME"
	OUT="$BASE/out_${STAMP}${SUF}"
	mkdir -p "$OUT" || { echo "nu pot crea $OUT"; exit 1; }
	echo "  Scriu in schimb in $OUT"
fi
echo "Rezultatele se salveaza in: $OUT"
MSGBUF=/private/var/log/rmb-msgbuf.log
PT=""
ping --apple-time -c 1 -t 2 127.0.0.1 >/dev/null 2>&1 && PT="--apple-time"
PI=1
ping -i 0.5 -c 2 -t 3 127.0.0.1 >/dev/null 2>&1 && PI=0.5

# Wi-Fi device (en0 on this laptop) and its router, independent of the default route.
WIFI_IF="$(networksetup -listallhardwareports 2>/dev/null | awk '/Hardware Port: (Wi-Fi|AirPort)/{getline; print $2; exit}')"
WIFI_IF="${WIFI_IF:-en0}"
wifi_gw() { ipconfig getoption "$WIFI_IF" router 2>/dev/null; }
default_if() { route -n get default 2>/dev/null | awk '/interface:/{print $2}'; }

# Kernel log (appended by Rembrandt660M disklog across boots): full copy gzipped + Wi-Fi lines.
save_msgbuf() {
	local name="$1"
	if [ -r "$MSGBUF" ]; then
		# the file is appended across boots: whole file once, then the last 3 MB
		if [ "$2" = tail ]; then tail -c 3000000 "$MSGBUF" | gzip -c > "$OUT/$name.gz"; else gzip -c "$MSGBUF" > "$OUT/$name.gz"; fi
		grep -a -E 'AirportMtkWlan|SimpleMtkWlan|mwx0|IO80211|WCL|wcl|Deauth|disassoc' "$MSGBUF" | tail -n 30000 | gzip -c > "$OUT/wifi_kernel_$name.gz"
		ls -la "$MSGBUF" "$OUT/$name.gz" "$OUT/wifi_kernel_$name.gz"
	else
		echo "lipseste $MSGBUF" | tee "$OUT/$name.missing"
	fi
}

wifi_link() {
	echo "== wifi link $(date +%H:%M:%S)"
	system_profiler SPAirPortDataType 2>/dev/null | sed -n '/Current Network Information:/,/Other Local Wi-Fi Networks:/p' | head -n 30
}

# In "all" and "lung" the test must run over Wi-Fi: iPhone USB / Ethernet as default route
# measured the iPhone instead (out_20261110_014542: default en1 172.20.10.1, ping 100% loss).
require_wifi() {
	local IF ans
	while :; do
		IF="$(default_if)"
		[ "$IF" = "$WIFI_IF" ] && [ -n "$(wifi_gw)" ] && return 0
		echo
		echo "ATENTIE: internetul merge acum prin '${IF:-nimic}', nu prin Wi-Fi ($WIFI_IF)."
		echo "Scoate iPhone-ul din USB (sau opreste Personal Hotspot pe USB), conecteaza Wi-Fi la reteaua de 5 GHz,"
		echo "apoi apasa Enter. Scrie s + Enter ca sa continui oricum."
		read -r ans
		[ "$ans" = "s" ] && return 1
	done
}

net_check() {
	local N="$OUT/net.txt" h IF GW WGW BIND=""
	IF="$(default_if)"
	GW="$(route -n get default 2>/dev/null | awk '/gateway:/{print $2}')"
	WGW="$(wifi_gw)"
	if [ "$MODE" != net ] && [ "$IF" = "$WIFI_IF" ]; then
		BIND="-I $WIFI_IF"
	fi
	echo "[retea] interfata implicita=$IF gateway=$GW wifi=$WIFI_IF router=$WGW (~4 minute) -> $N"
	{
		echo "== date"; date
		echo "== default route"; route -n get default
		echo "default interface=$IF gateway=$GW wifi_if=$WIFI_IF wifi_router=$WGW"
		echo "== hardware ports"; networksetup -listallhardwareports
		echo "== ifconfig"; ifconfig
		echo "== routes"; netstat -rn
		echo "== dns"; scutil --dns
		echo "== nwi"; scutil --nwi
		echo "== proxy"; scutil --proxy
		echo "== wifi"; system_profiler SPAirPortDataType
		echo "== tcp stats before"; netstat -s -p tcp
		echo "== ip stats before"; netstat -s -p ip
		echo "== ping gateway"; [ -n "$GW" ] && ping -c 5 -t 8 "$GW"
		echo "== ping 1.1.1.1"; ping -c 10 -t 15 1.1.1.1
		echo "== ping6 2606:4700:4700::1111"; ping6 -c 3 2606:4700:4700::1111
		echo "== mtu ipv4 (DF): gateway = link + router, 1.1.1.1 = internet path"
		for d in "$GW" 1.1.1.1; do
			[ -n "$d" ] || continue
			for s in 1472 1464 1452 1432 1400 1300 1200; do
				if ping -D -c 2 -t 4 -s "$s" "$d" >/dev/null 2>&1; then echo "payload $s to $d OK"; else echo "payload $s to $d FAIL"; fi
			done
		done
		# idle jitter: 120 pings / 60 s to the router; spikes here = radio off channel or queueing
		echo "== ping router idle 60 s ($GW)"
		[ -n "$GW" ] && ping $PT -i $PI -c 120 "$GW" > "$OUT/ping_idle.txt" 2>&1
		tail -3 "$OUT/ping_idle.txt"
	} > "$N" 2>&1
	{
		echo "== sites"
		{
			printf '%s\n' google.com youtube.com wikipedia.org cloudflare.com apple.com facebook.com instagram.com netflix.com emag.ro digi24.ro
			[ -f "$HERE/site_uri.txt" ] && sed -e 's/\r$//' -e 's/#.*//' -e 's#^[a-zA-Z]*://##' -e 's#/.*##' -e 's/[[:space:]]//g' "$HERE/site_uri.txt"
		} | awk 'NF && !seen[$0]++' | while read -r h; do
			echo "-- $h"
			echo "dns router A:    $(dig +time=3 +tries=1 +short A "$h" | tr '\n' ' ')"
			echo "dns router AAAA: $(dig +time=3 +tries=1 +short AAAA "$h" | tr '\n' ' ')"
			echo "dns 1.1.1.1 A:   $(dig @1.1.1.1 +time=3 +tries=1 +short A "$h" | tr '\n' ' ')"
			echo "system resolver: $(dscacheutil -q host -a name "$h" | awk '/address/{print $2}' | tr '\n' ' ')"
			for v in 4 6; do
				curl -"$v" -sS -o /dev/null --max-time 15 -w "ipv$v http=%{http_code} ip=%{remote_ip} dns=%{time_namelookup} tcp=%{time_connect} tls=%{time_appconnect} first=%{time_starttransfer} total=%{time_total} bytes=%{size_download}\n" "https://$h/" 2>&1
			done
			curl -sS -o /dev/null --max-time 20 -w "auto http=%{http_code} ip=%{remote_ip} total=%{time_total} bytes=%{size_download}\n" "https://$h/" 2>&1
			# whole page with redirects: a page that does not load shows up here as a timeout or few bytes
			curl -L -sS -o /dev/null --max-time 30 -A "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)" -w "page http=%{http_code} bytes=%{size_download} speed=%{speed_download}B/s total=%{time_total} url=%{url_effective}\n" "https://$h/" 2>&1
		done
		wifi_link
		echo "== networkQuality (viteza, simultan) $BIND"
		ping $PT -c 90 -i $PI "${GW:-1.1.1.1}" > "$OUT/ping_during_speedtest.txt" 2>&1 &
		networkQuality $BIND -v
		wifi_link
		echo "== networkQuality (viteza, secvential: download apoi upload) $BIND"; networkQuality $BIND -s -v
		wait
		wifi_link
		echo "== ping router in timpul testelor de viteza"; tail -4 "$OUT/ping_during_speedtest.txt"
		echo "== tcp stats after"; netstat -s -p tcp
		echo "== ip stats after"; netstat -s -p ip
	} >> "$N" 2>&1
	sync
	grep -E '^-- |^ipv|^auto|^page|^dns|Uplink capacity|Downlink capacity|Responsiveness:|Idle Latency:|payload|packet loss|round-trip|Transmit Rate|MCS Index' "$N"
}

long_check() {
	local N="$OUT/lung.txt" WGW i
	WGW="$(wifi_gw)"
	echo "[lung] 10 minute pe Wi-Fi: ping router $WGW la 0.5 s + viteza la fiecare 2 minute -> $N"
	echo "Foloseste laptopul normal (browser, video). Nu inchide Terminalul."
	ping $PT -i $PI -c $([ "$PI" = 1 ] && echo 600 || echo 1200) "$WGW" > "$OUT/ping_lung.txt" 2>&1 &
	{
		for i in 1 2 3 4 5; do
			echo "== runda $i $(date +%H:%M:%S) default=$(default_if)"
			wifi_link
			networkQuality -I "$WIFI_IF" -c 2>&1 | head -c 4000; echo
			sleep 95
		done
	} > "$N" 2>&1
	wait
	tail -3 "$OUT/ping_lung.txt" | tee -a "$N"
	grep -c -E 'timeout|No route|is down' "$OUT/ping_lung.txt" | sed 's/^/ping pierdute sau fara ruta: /' | tee -a "$N"
}

# End of every mode: small pieces of big files, manifest, one archive, copy on the stick if it fits.
finish() {
	local f n z TGZ FREE_KB SZ_KB STICK=""
	sync
	# Files > 240 KB are also split (originals kept): the copy that brought out_20261110_014542
	# to Windows left exactly the files > 376 KB at 0 bytes. Rejoin: cat name.part-* > name
	for f in "$OUT"/*; do
		[ -f "$f" ] || continue
		case "$f" in *.part-*) continue ;; esac
		if [ "$(wc -c < "$f")" -gt 245760 ]; then
			split -b 240k "$f" "$f.part-"
		fi
	done
	# MANIFEST: size + SHA-256 of every file, to detect copies that arrive empty or truncated.
	( cd "$OUT" && { echo "== $(date)  $OUT"; ls -la; echo "== sha256"; shasum -a 256 -- * 2>/dev/null; } > MANIFEST.txt )
	z=0
	for f in "$OUT"/*; do
		[ -f "$f" ] && [ ! -s "$f" ] && { echo "ATENTIE: fisier gol: $(basename "$f")"; z=$((z + 1)); }
	done
	n="$(basename "$OUT")"
	TGZ="$BASE/$n.tgz"
	tar -czf "$TGZ" --exclude '*.part-*' -C "$BASE" "$n" 2>/dev/null || echo "ATENTIE: nu am putut crea $TGZ"
	sync
	du -sh "$OUT"
	[ -f "$TGZ" ] && ls -la "$TGZ"
	# Copy on the stick only with >= 2 MB left afterwards (A: is 96 MB and almost full).
	if [ -f "$TGZ" ] && [ "$HERE" != "$BASE" ]; then
		FREE_KB="$(df -k "$HERE" 2>/dev/null | awk 'NR==2{print $4}')"
		SZ_KB=$(( $(wc -c < "$TGZ") / 1024 + 1 ))
		if [ -n "$FREE_KB" ] && [ "$FREE_KB" -gt $((SZ_KB + 2048)) ] && [ ! -e "$HERE/$n.tgz" ]; then
			if cp "$TGZ" "$HERE/$n.tgz" && sync && cmp -s "$TGZ" "$HERE/$n.tgz"; then
				STICK="$HERE/$n.tgz"; echo "copie verificata pe stick: $STICK"
			else
				echo "ATENTIE: copia pe stick nu e identica cu cea de pe Desktop"
			fi
		else
			echo "pe stick nu incape ($((${FREE_KB:-0} / 1024)) MB liberi)"
		fi
	fi
	echo
	echo "GATA. Pe Desktop: folderul $n si arhiva $n.tgz (fisiere goale: $z)."
	if [ -n "$STICK" ]; then
		echo "Trimite arhiva de pe stick ($n.tgz): Windows o citeste direct, fara copiere din macOS."
	else
		echo "Copiaza TOT folderul Desktop/$n: bucatile *.part-* ajung intregi chiar daca fisierele mari ies goale."
	fi
	{ command -v open >/dev/null 2>&1 && open -R "$TGZ" 2>/dev/null; } || true
}

if [ "$MODE" = net ]; then
	net_check
	save_msgbuf rmb-msgbuf_after_net.log tail
	finish
	exit 0
fi

if [ "$MODE" = lung ]; then
	require_wifi
	{ date; sw_vers; uname -a; sysctl kern.boottime; nvram boot-args; kextstat | grep -i -E 'mtk|airport|lilu|rembrandt'; } > "$OUT/system.txt" 2>&1
	long_check
	save_msgbuf rmb-msgbuf.log
	finish
	exit 0
fi

echo "[1/6] log kext -> $OUT"
save_msgbuf rmb-msgbuf.log
sync
echo "[2/6] sistem si kext-uri"
{ date; sw_vers; uname -a; sysctl kern.boottime; nvram boot-args; kextstat | grep -i -E 'rembrandt|amd|lilu|AppleGraphics|mtk|airport'; } > "$OUT/system.txt" 2>&1
echo "[3/6] ioreg"
ioreg -l -w0 > /tmp/rmb_ioreg_full.txt 2>&1
grep -E '\+-o (AMDRadeonX6000_|IOAccel|AGDP|AppleGraphics)|IOUserClientCreator' /tmp/rmb_ioreg_full.txt > "$OUT/ioreg_userclients.txt"
gzip -c /tmp/rmb_ioreg_full.txt > "$OUT/ioreg_full.txt.gz"
echo "[4/6] system_profiler, log WindowServer/Metal, rapoarte panic (poate dura ~1 minut)"
system_profiler SPDisplaysDataType > "$OUT/spdisplays.txt" 2>&1
system_profiler -xml SPDisplaysDataType > "$OUT/spdisplays.xml" 2>&1
log show --last boot --style compact --predicate 'process == "WindowServer" OR subsystem == "com.apple.Metal" OR process == "displaypolicyd"' 2>/dev/null \
	| grep -i -E 'metal|mtl|gpu|accel|render|software|fallback|display|fail|error|timeout|restart' | tail -n 4000 | gzip -c > "$OUT/log_ws_metal.txt.gz"
log show --last boot --style compact --predicate 'subsystem == "com.apple.wifi" OR process == "airportd" OR process == "wifid" OR process == "wifip2pd"' 2>/dev/null \
	| grep -i -E 'disassoc|deauth|leave|link down|linkdown|roam|reason|fault|missed beacon|scan.*(fail|busy)|error' | tail -n 6000 | gzip -c > "$OUT/log_wifi_daemons.txt.gz"
ls -la /Library/Logs/DiagnosticReports/ > "$OUT/diagnosticreports_ls.txt" 2>&1
find /Library/Logs/DiagnosticReports -maxdepth 1 -name '*.panic' -mtime -2 -exec cp {} "$OUT/" \; 2>/dev/null
sync
echo "[5/6] retea Wi-Fi"
require_wifi
net_check
save_msgbuf rmb-msgbuf_after_net.log tail
echo "[6/6] test Metal (max ~20 s). Daca ecranul ingheata, asteapta 1 minut si trimite ce exista in $OUT."
cp "$HERE/rmb_metaltest" /tmp/rmb_metaltest && chmod +x /tmp/rmb_metaltest
/tmp/rmb_metaltest > "$OUT/metaltest.txt" 2>&1
echo "exit=$?" >> "$OUT/metaltest.txt"
cat "$OUT/metaltest.txt"
ioreg -l -w0 -r -c IOAccelerator | grep -E 'PerformanceStatistics' > "$OUT/perfstats_after_metaltest.txt" 2>&1
finish
