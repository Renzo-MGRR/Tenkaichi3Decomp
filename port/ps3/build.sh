#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$BASH_SOURCE[0]")/../.." && pwd)"
cd "$ROOT"
source port/ps3/env.sh
mkdir -p port/build/ps3 port/build/obj_ps3
bash port/ps3/halbuild.sh
python3 port/tools/ps3all.py
python3 port/tools/ps3data.py
python3 port/tools/ps3port.py
python3 port/tools/ps3link.py
python3 port/tools/ps3dat.py
