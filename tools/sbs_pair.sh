#!/bin/bash
# usage: tools/sbs_pair.sh name doc.mmd  [BASE=rev, default HEAD]
# -> samples/sbs/name_before.png (BASE built in a worktree at $WT, default /tmp/cmm-wt) and name_after.png (this tree)
set -e
R=$(cd "$(dirname "$0")/.." && pwd); cd "$R"; H=$(git rev-parse "${BASE:-HEAD}"); d=$(realpath "$2"); WT=${WT:-/tmp/cmm-wt}
[ -d "$WT" ] || git worktree add -q --detach "$WT" "$H"
b=$(cd "$WT" && git switch -q --detach "$H" && make -s >/dev/null 2>&1 && python3 "$R/tools/docs_sbs.py" "$d" | tail -1)
cp "$b" "samples/sbs/$1_before.png"
a=$(python3 tools/docs_sbs.py "$d" | tail -1); cp "$a" "samples/sbs/$1_after.png"; echo samples/sbs/$1_{before,after}.png
