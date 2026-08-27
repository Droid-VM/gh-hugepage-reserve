#!/system/bin/sh
set -x
while [ -z "$(ls -A /data/adb/modules/)" ]; do
	sleep 1
done
DIR=/data/adb/modules/gh-hugepage-reserve
rm -f "$DIR"/{load,dmesg}.log
exec 1<>"$DIR"/load.log
exec 2<>"$DIR"/load.log
if [ -f "$DIR/disable" ]; then
	exit 0
fi
if [ -f "$DIR/stamp" ]; then
	touch "$DIR/crash"
	rm -f "$DIR/stamp"
fi
if [ -f "$DIR/crash" ]; then
	touch "$DIR/disable"
	exit 0
fi
touch "$DIR/stamp"
dmesg -w &> "$DIR/dmesg.log" &
# Preflight + insmod live in load.sh so the DroidVM app's runtime "Enable"
# reproduces exactly this configuration (see load.sh). Everything above is
# boot-only: the crash watchdog, the logs, and the dmesg tap.
# GH_BOOT: this is the blocking boot stage, so load.sh may spend its configured
# boot_acquire_wait here holding userspace back while the acquire works. The
# app's runtime enable calls the same script without it and never blocks.
GH_BOOT=1 sh "$DIR/load.sh"
exit 0
