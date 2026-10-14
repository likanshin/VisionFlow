#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${1:-build/debug}"

# The project runs only in its dedicated environment; never fall back to base
# or system Python. This script does not activate environments or edit the shell.
if [[ "${CONDA_DEFAULT_ENV:-}" != "visionflow" || -z "${CONDA_PREFIX:-}" ]]; then
    printf '%s\n' 'Activate the project environment first: conda activate visionflow' >&2
    exit 1
fi
python_bin="$CONDA_PREFIX/bin/python"
if [[ ! -x "$python_bin" ]]; then
    printf 'Missing project Python: %s\n' "$python_bin" >&2
    exit 1
fi
if ! "$python_bin" -c 'import cv2, numpy'; then
    printf '%s\n' 'Project dependencies are missing. Recreate the environment using environment.yml.' >&2
    exit 1
fi

printf 'VisionFlow Python: %s\n' "$python_bin"
cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    -DPython3_EXECUTABLE="$python_bin"
cmake --build "$build_dir" --parallel 4
cmake -E chdir "$build_dir" ctest --output-on-failure
"$python_bin" scripts/demo.py --bin-dir "$build_dir/bin"
