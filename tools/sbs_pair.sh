#!/bin/bash
# usage: tools/sbs_pair.sh name doc.mmd  [BASE=rev, default HEAD]
# -> samples/sbs/name_before.png (BASE built in a worktree at $WT, default /tmp/mmd-wt) and name_after.png (this tree)
set -e
R=$(cd "$(dirname "$0")/.." && pwd); cd "$R"; H=$(git rev-parse "${BASE:-HEAD}"); d=$(realpath "$2"); WT=${WT:-/tmp/mmd-wt}
[ -d "$WT" ] || git worktree add -q --detach "$WT" "$H"
(cd "$WT" && git switch -q --detach "$H" && make -s build/mmd >/dev/null 2>&1)
# ours|mermaid-cli pair for tests/docs files that mermaid-cli renders, else ours alone
pair() { o=; case "$d" in "$R"/tests/docs/*) o=$(cd "$1" && python3 "$R/tools/docs_sbs.py" "$d" | tail -1);; esac; if [ -f "$o" ]; then cp "$o" "$2"; else "$R/tools/render_png.sh" "$d" "$2" "$1/build/mmd"; fi; }
pair "$WT" "samples/sbs/$1_before.png"; pair "$R" "samples/sbs/$1_after.png"; echo samples/sbs/$1_{before,after}.png
