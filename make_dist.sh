#!/bin/sh
# Same as: python make_dist.py (works on all platforms, also from cmd)
cd "$(dirname "$0")" || exit 1
# the first Python that has the build packages
for py in python3 python; do
    "$py" -c "import ziglang, capstone" 2>/dev/null && exec "$py" make_dist.py "$@"
done
echo "Python 3 with the packages capstone and ziglang is needed (pip install capstone ziglang)." >&2
exit 1
