#!/usr/bin/env bash
# Per-thread CPU use of a running process over an interval:
#   tests/thread_cpu.sh <pid> [seconds]
# Prints % of one core per thread (name from /proc), busiest first.
pid=$1; secs=${2:-10}; hz=$(getconf CLK_TCK)
snap() { for t in /proc/$pid/task/*; do
    read -r st < "$t/stat" 2>/dev/null || continue
    name=$(cat "$t/comm" 2>/dev/null); rest=${st##*) }; set -- $rest
    echo "${t##*/} $(( ${12} + ${13} )) $name"; done; }
a=$(snap); sleep "$secs"; b=$(snap)
join <(echo "$a" | sort) <(echo "$b" | sort) | awk -v hz=$hz -v s=$secs \
  '{ d=$4-$2; if (d>0) printf "%6.1f%%  %s %s\n", 100*d/hz/s, $1, $3 }' | sort -rn | head -20
