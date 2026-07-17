#!/bin/sh
# xfce4-genmon-plugin consumer for the zenmonitor-cli daemon snapshot.
#
# Usage in a genmon panel item (Command field):
#   zenmonitor-genmon.sh "CPU Temperature (tCtl)" "Avg 1m"
#
# Arg 1: exact sensor label as written in the snapshot (default: first sensor)
# Arg 2: column to display - "value" or one of the configured average
#        windows, e.g. "Avg 1m" (default: value)
#
# The snapshot is produced by:  zenmonitor-cli --daemon --average 1m,5m
# Path resolution: $ZENMONITOR_SNAPSHOT, else $XDG_RUNTIME_DIR/zenmonitor.snapshot,
# else /tmp/zenmonitor.snapshot.

snapshot="${ZENMONITOR_SNAPSHOT:-${XDG_RUNTIME_DIR:-/tmp}/zenmonitor.snapshot}"
sensor="$1"
column="${2:-value}"

if [ ! -r "$snapshot" ]; then
    echo "<txt>n/a</txt>"
    echo "<tool>zenmonitor daemon not running ($snapshot)</tool>"
    exit 0
fi

awk -F'\t' -v sensor="$sensor" -v col="$column" '
    NR == 2 {
        sub(/^# /, "", $0)
        n = split($0, h, "\t")
        for (i = 1; i <= n; i++)
            if (h[i] == col) ci = i
        if (sensor == "") skip_sensor = 1
        next
    }
    NR > 2 && (skip_sensor || $1 == sensor) {
        if (ci == "") { print "<txt>?col</txt>"; exit }
        v = $ci
        if (v == "") { print "<txt>--</txt>"; exit }
        printf "<txt>%.1f</txt>\n", v
        printf "<tool>%s — %s: %.2f</tool>\n", $1, col, v
        found = 1
        exit
    }
    END {
        if (!found) {
            print "<txt>n/a</txt>"
            print "<tool>sensor not found: " sensor "</tool>"
        }
    }
' "$snapshot"
