#!/bin/bash
# Per-phase instruction counts (callgrind inclusive Ir). usage: bench/phases.sh BIN INPUT
B=${1:-build/mmd}; I=${2:-bench/inputs/medium.mmd}; o=$(mktemp)
valgrind --tool=callgrind --callgrind-out-file=$o $B $I >/dev/null 2>&1
callgrind_annotate --inclusive=yes $o 2>/dev/null > $o.txt
for f in mmd_render_svg flow_parse flow_layout dagre_rank network_simplex_dagre dagre_order bk flow_svg; do
  printf '%-22s %s\n' $f "$(grep -E ":$f( |'|$)" $o.txt | head -1 | awk '{print $1}')"; done
rm -f $o $o.txt
