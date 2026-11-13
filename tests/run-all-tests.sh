#!/usr/bin/env bash
# Keep this entry point current whenever tests or their prerequisites change.
set -euo pipefail
umask 077
ulimit -c 0

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(dirname -- "$script_dir")"
test_build_dir="/tmp/darkspeak-all-tests-${UID}"
test_python="python3"
build_jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
dry_run=false
runtime_dir=""

usage() {
    cat <<'EOF'
Usage: tests/run-all-tests.sh [options] [-- extra CMake arguments]

Configure, build and run all registered unit, functional and acceptance tests.
Requires Linux, Python 3.12+, Tor/tor-gencert and the project's C++/Qt dependencies.
Creates a private Python virtualenv and installs tests/mcp/requirements.txt.
Builds the application too: the QML smoke tests execute it.

Options:
  --build-dir DIR  Build directory (default: /tmp/darkspeak-all-tests-$UID)
  --python PATH    Python interpreter used to create the virtualenv
  --jobs N         Parallel build jobs (default: CMAKE_BUILD_PARALLEL_LEVEL or 3)
  --dry-run        Print commands without creating files, installing or building
  -h, --help      Show this help

Tests run sequentially, with their CTest deadlines (180 seconds if unspecified).
CTest runs the entire catalog, including future registered acceptance scenarios.
Any dependency, configure, build or test failure produces a nonzero exit status.
EOF
}

fail() { printf 'run-all-tests: %s\n' "$*" >&2; exit 2; }
while (($#)); do
    case "$1" in
        --build-dir|--python|--jobs)
            (($# >= 2)) && [[ -n "$2" ]] || fail "$1 requires a value"
            case "$1" in
                --build-dir) test_build_dir="$2" ;;
                --python) test_python="$2" ;;
                --jobs) build_jobs="$2" ;;
            esac
            shift 2
            ;;
        --dry-run) dry_run=true; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) fail "Unknown option: $1 (use --help)" ;;
    esac
done
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"
[[ "$(uname -s)" == Linux ]] || fail "The isolated test runtime requires Linux"
for dependency in cmake ctest tor tor-gencert ffmpeg "$test_python"; do
    command -v "$dependency" >/dev/null || fail "Missing dependency: $dependency"
done
"$test_python" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' \
    || fail "Python 3.12 or newer is required"
# Resolve relative build paths before changing directories for a stable invocation.
test_build_dir="$("$test_python" -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$test_build_dir")"
venv_python="$test_build_dir/python-venv/bin/python"

run() {
    if "$dry_run"; then
        printf '  '
        printf '%q ' "$@"
        printf '\n'
    else
        "$@"
    fi
}
cleanup() {
    if ! "$dry_run" && [[ -n "$runtime_dir" && -d "$runtime_dir" ]]; then
        rm -rf -- "$runtime_dir"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cd -- "$project_dir"
run mkdir -p -- "$test_build_dir"
if [[ ! -x "$venv_python" ]]; then
    run "$test_python" -m venv "$test_build_dir/python-venv"
fi
run "$venv_python" -m pip install --quiet --disable-pip-version-check --no-input \
    -r "$script_dir/mcp/requirements.txt"

# Extra toolchain arguments are allowed, but cannot switch off mandatory suites.
run cmake -S "$project_dir" -B "$test_build_dir" -DCMAKE_BUILD_TYPE=Debug "$@" \
    -DBUILD_TESTING=ON -DDSFM_BUILD_TESTS=ON -DDSFM_BUILD_EXAMPLE=OFF \
    -DDARKSPEAK_WITH_MCP=ON -DDARKSPEAK_WITH_TEST_MCP=ON \
    -DDS_ENABLE_TOR_TESTS=ON -DPython3_EXECUTABLE="$venv_python"
# The default build includes every test executable and the app used by QML smoke
# checks. Private-Tor privacy, multi-identity, resume, deletion, transfer-decision
# and corrupted-partial scenarios
# and database crash-recovery checks use this build and are discovered below.
# Media-preview codec, managed-cache, and private-Tor preview scenarios use Qt's existing JPEG plugin;
# no additional Qt module or system thumbnailer is needed for image previews.
# Video tests require FFmpeg and a Linux kernel with Landlock and seccomp.
# The startup dependency-dialog and persistent opt-out tests are also discovered.
# No explicit target allowlist can accidentally omit a future test binary.
run cmake --build "$test_build_dir" --parallel "$build_jobs"

if "$dry_run"; then
    runtime_dir="/tmp/darkspeak-test-run.DRY_RUN"
else
    runtime_dir="$(mktemp -d /tmp/darkspeak-test-run.XXXXXXXX)"
fi
run mkdir -p -- "$runtime_dir/home" "$runtime_dir/config" "$runtime_dir/data" \
    "$runtime_dir/cache" "$runtime_dir/runtime"
run env HOME="$runtime_dir/home" XDG_CONFIG_HOME="$runtime_dir/config" \
    XDG_DATA_HOME="$runtime_dir/data" XDG_CACHE_HOME="$runtime_dir/cache" \
    XDG_RUNTIME_DIR="$runtime_dir/runtime" DBUS_SESSION_BUS_ADDRESS= \
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    DARKSPEAK_MCP_TEST_PYTHON="$venv_python" \
    ctest --test-dir "$test_build_dir" --parallel 1 --timeout 180 \
    --output-on-failure --no-tests=error
