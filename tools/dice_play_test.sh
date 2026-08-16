#!/bin/sh
# Play a real 440 Hz tone to the DICE device (S16_LE @ 44100) for a few
# seconds.  Unlike a stream of zeros this produces actual audio so the
# device can lock and its meter LEDs light up.  Captures new dmesg output
# (dice + fwohci lines) for the diagnostic build.
#
# Usage: dice_play_test.sh [device] [seconds]
#   device  /dev/dsp1 (default)
#   seconds 6
set -u
DSP="${1:-/dev/dsp1}"
SECS="${2:-6}"

echo "=== sndstat before ==="
cat /dev/sndstat 2>/dev/null | grep -A6 "dsp1"

MARK=$(dmesg | wc -l)

# Generate a 440 Hz square tone as raw S16_LE stereo @ 44100.
# 440 Hz at 44100 = 100 samples per half period.  Square wave: +16000/-16000.
(
	perl -e '
		$rate = 44100;
		$freq = 440;
		$half = int($rate / $freq / 2);
		$amp  = 16000;
		$n    = $rate * $ARGV[0];   # samples for N seconds
		$out = "";
		for ($s = 0; $s < $n; $s++) {
			$v = (int($s / $half) % 2 == 0) ? $amp : -$amp;
			$out .= pack("s<", $v) . pack("s<", $v);
		}
		print $out;
	' "${SECS}" > "${DSP}"
) &
GEN=$!
sleep "${SECS}"
kill "${GEN}" 2>/dev/null
wait "${GEN}" 2>/dev/null

echo "=== new dmesg (dice + fwohci) ==="
dmesg | sed -n "$((MARK + 1)),\$p" | grep -E "dice|fwohci|pcm1" | head -60

echo "=== sndstat after ==="
cat /dev/sndstat 2>/dev/null | grep -A6 "dsp1"
exit 0
