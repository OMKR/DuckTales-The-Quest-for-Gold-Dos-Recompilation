#!/bin/sh
# Development loop: build, run headless with scripted keys, and if the run
# stopped at a call to unknown code, add it to tools/extra_entries.txt and
# repeat (up to N times).
cd "$(dirname "$0")"
N=${N:-10}
for i in $(seq 1 $N); do
    ./build.sh > build/build_log.txt 2>&1 || { grep -E "error|Error" build/build_log.txt | head -8; exit 1; }
    rm -f build/missing_entries.txt
    (cd build && DT_HEADLESS=1 DT_KEYS="${DT_KEYS:-50:n}" DT_MAX_FRAMES=${FRAMES:-600} timeout 120 \
        ./ducktales_dos.exe ../files > out.txt 2> err.txt)
    if [ -s build/missing_entries.txt ]; then
        echo "adding: $(cat build/missing_entries.txt)"
        cat build/missing_entries.txt >> tools/extra_entries.txt
    else
        tail -6 build/err.txt
        break
    fi
done
