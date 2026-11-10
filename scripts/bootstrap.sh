#!/usr/bin/env bash
# Build the published native integration on Ubuntu 24.04 x86-64 / WSL2.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:---test}
case "$mode" in --check|--test|--native-test|--build-only) ;; *) echo 'Usage: bash scripts/bootstrap.sh [--check|--test|--native-test|--build-only]' >&2; exit 2;; esac
if [[ "$root" =~ [[:space:]] ]]; then echo 'Use a checkout path without spaces; upstream build scripts require it.' >&2; exit 2; fi
for tool in cc make cmake ninja pkg-config python3 rsync; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool. See docs/quickstart.md." >&2; exit 2; }
done
python3 - <<'PY'
import re,subprocess
version=tuple(map(int,re.search(r'(\d+)\.(\d+)\.(\d+)',subprocess.check_output(['cmake','--version'],text=True)).groups()))
if version<(3,23,0):raise SystemExit('CMake >=3.23 is required; see docs/quickstart.md.')
PY
for requirement in 'openssl >= 3.0' 'jansson >= 2.14' 'sqlite3 >= 3.31.1'; do
  pkg-config --exists "$requirement" || { echo "Missing development dependency: $requirement. See docs/quickstart.md." >&2; exit 2; }
done
for file in BSL/CMakeLists.txt BSL/deps/QCBOR/CMakeLists.txt BSL/deps/mlib/Makefile BSL/deps/unity/CMakeLists.txt bplib/CMakeLists.txt bpnode/CMakeLists.txt cfs-bundle/cfe/CMakeLists.txt cfs-bundle/osal/CMakeLists.txt cfs-bundle/psp/CMakeLists.txt cfs-bundle/apps/sch_lab/CMakeLists.txt; do
  [[ -f "$root/$file" ]] || { echo "Missing source dependency: $file. Clone with --recurse-submodules or use the complete-source release." >&2; exit 2; }
done
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'The supported quick-start target is Linux x86-64, including Ubuntu under WSL2.' >&2; exit 2; }
if [[ "$mode" == --check ]]; then echo 'PASS: required tools, package versions and source dependencies are present.'; exit 0; fi
build=${BPSEC_BUILD_DIR:-$root/build}
venv=${BPSEC_VENV_DIR:-$root/.venv}
mkdir -p "$build"
python3 -m venv "$venv"
"$venv/bin/python" -m pip install -r "$root/client-test/requirements.txt"
(
  cd "$root/BSL"
  ./build.sh deps
)
version_args=()
if [[ ! -e "$root/BSL/.git" ]]; then version_args=(-DPROJECT_VERSION=0.0.0); fi
cmake -S "$root/BSL" -B "$root/BSL/build/default" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$root/BSL/testroot/usr" \
  -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_UNITTEST=OFF -DTEST_MEMCHECK=OFF \
  -DBUILD_DOCS_API=OFF -DBUILD_DOCS_MAN=OFF -DBSL_LOG_PLAINTEXT_ENABLE=OFF "${version_args[@]}"
cmake --build "$root/BSL/build/default" -j4
# This also generates the cFS headers needed by the BPNode regression targets.
GIT_DIR=/dev/null "$venv/bin/python" "$root/demo/five_node.py" build --build-dir "$build/native"
# Register Python regressions with the interpreter that has the demo dependencies.
cmake -S "$root/integration" -B "$build/native/integration" \
  -DBSL_BUILD="$root/BSL/build/default" -DCFS_BUILD="$build/native/cfs" \
  -DPython3_EXECUTABLE="$venv/bin/python"
cmake --build "$build/native/integration" -j4
if [[ "$mode" != --build-only ]]; then
  "$venv/bin/python" "$root/docs/check_documentation.py"
  ctest --test-dir "$build/native/integration" --output-on-failure --timeout 60
fi
if [[ "$mode" == --native-test ]]; then
  mkdir -p "$build/runs"
  run=$(mktemp -d "$build/runs/baseline.XXXXXX")
  "$venv/bin/python" "$root/demo/test_five_node.py" --build-dir "$build/native" --run-dir "$run" --port-base 34000 --timeout 90
fi
printf '\nBuild complete: %s\nPython environment: %s\n' "$build/native" "$venv"
