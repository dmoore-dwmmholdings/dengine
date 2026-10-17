#!/usr/bin/env bash
# DENG-436: same-process lavapipe evidence; never changes the fixture's work or deadlines.
# Invoke from the repository root with a built test executable and an output directory.
set -euo pipefail
exe=$(realpath "$1")
out=$(realpath -m "$2")
perf_bin=${PERF_BIN:-perf}
mkdir -p "${out}/jit" "${out}/control-cache" "${out}/profile-cache"
export MESA_SHADER_CACHE_DIR="${out}/profile-cache"

# The isolated directory starts empty; the second process uses exactly its resulting contents.
# Do not disable caches, validation, rasterization, shader work or driver worker threads.
if [ -n "$(find "${out}/control-cache" "${out}/profile-cache" -type f -print -quit)" ]; then
    echo "diagnostic cache must initially be empty" >&2
    exit 1
fi
if [ -n "${LP_PERF:-}" ] || [ -n "${LP_NO_RAST:-}" ]; then
    echo "work-disabling driver flags are not permitted in this diagnostic" >&2
    exit 1
fi
if [ -n "${MESA_SHADER_CACHE_DISABLE:-}" ]; then
    echo "MESA_SHADER_CACHE_DISABLE must be unset for the controlled cold/warm comparison" >&2
    exit 1
fi
{
    date -u --iso-8601=ns
    git rev-parse HEAD
    uname -a
    lscpu
    free -h
    "${perf_bin}" --version
    cat /proc/sys/kernel/perf_event_paranoid
    dpkg-query -W mesa-vulkan-drivers vulkan-validationlayers 'libllvm*'
    vulkaninfo --summary
    sha256sum /usr/lib/x86_64-linux-gnu/libvulkan_lvp.so
    printf 'VK_DRIVER_FILES=%s\nVK_ICD_FILENAMES=%s\nMESA_SHADER_CACHE_DIR=%s\n' \
        "${VK_DRIVER_FILES:-}" "${VK_ICD_FILENAMES:-}" "${MESA_SHADER_CACHE_DIR}"
    printf 'LP_NUM_THREADS=%s\nLP_PERF=%s\nLP_NO_RAST=%s\n' \
        "${LP_NUM_THREADS:-<unset>}" "${LP_PERF:-<unset>}" "${LP_NO_RAST:-<unset>}"
    printf 'CTEST_PARALLEL_LEVEL=%s\nGITHUB_RUN_ID=%s\nGITHUB_JOB=%s\nRUNNER_NAME=%s\n' \
        "${CTEST_PARALLEL_LEVEL:-<unset>}" "${GITHUB_RUN_ID:-<unset>}" \
        "${GITHUB_JOB:-<unset>}" "${RUNNER_NAME:-<unset>}"
} > "${out}/metadata.txt" 2>&1

# Validate this runner's permissions and all recording flags before any costly control/profile run.
"${perf_bin}" record --clockid mono -g --call-graph dwarf,8192 -F 49 -e cpu-clock:u \
    -o "${out}/preflight.data" -- true > "${out}/preflight.txt" 2>&1

# Separate unchanged, untraced/unprofiled controls distinguish observer overhead. The two pairs
# have different initially empty cache directories; each warm process reuses its own cold cache.
status=0
unset DE_VK_SYNC_DIAGNOSTICS JIT_SYMBOL_MAP_DIR
export MESA_SHADER_CACHE_DIR="${out}/control-cache"
for state in cold warm; do
    find "${MESA_SHADER_CACHE_DIR}" -type f -printf '%s %p\n' \
        > "${out}/control-${state}-cache-before.txt"
    set +e
    /usr/bin/time -v timeout --signal=INT --kill-after=30s 300s \
        "${exe}" --filter=clustered_lights.clusters_assignment_cpu_ref_vulkan \
        > "${out}/control-${state}-test.txt" 2>&1
    rc=$?
    set -e
    printf 'control-%s exit=%s\n' "${state}" "${rc}" | tee -a "${out}/results.txt"
    [ "${rc}" -eq 0 ] || status=1
    find "${MESA_SHADER_CACHE_DIR}" -type f -printf '%s %p\n' \
        > "${out}/control-${state}-cache-after.txt"
done

export DE_VK_SYNC_DIAGNOSTICS=1
export MESA_SHADER_CACHE_DIR="${out}/profile-cache"
export JIT_SYMBOL_MAP_DIR="${out}/jit"
for state in cold warm; do
    find "${MESA_SHADER_CACHE_DIR}" -type f -printf '%s %p\n' \
        > "${out}/${state}-cache-before.txt"
    set +e
    # Mesa documents perf -g/JIT maps. Monotonic timestamps correlate with vk-sync/lx-diag.
    # The outer diagnostic budget does not alter the original 120s/10s in-process waits.
    timeout --signal=INT --kill-after=30s 300s \
        "${perf_bin}" record --clockid mono -g --call-graph dwarf,8192 -F 49 -e cpu-clock:u \
        -o "${out}/${state}.data" -- bash -c 'echo target_pid=$$; exec "$@"' -- \
        "${exe}" --filter=clustered_lights.clusters_assignment_cpu_ref_vulkan \
        > "${out}/${state}-test.txt" 2>&1
    rc=$?
    set -e
    printf '%s exit=%s\n' "${state}" "${rc}" | tee -a "${out}/results.txt"
    [ "${rc}" -eq 0 ] || status=1
    pid=$(sed -n 's/^target_pid=\([0-9]*\)$/\1/p' "${out}/${state}-test.txt" | head -n 1)
    if [ -n "${pid}" ]; then
        for map in /tmp/perf-"${pid}".map /tmp/perf-"${pid}".map.asm; do
            [ ! -f "${map}" ] || cp "${map}" "${out}/jit/"
        done
    fi
    # Conversion failures are diagnostic failures, but never prevent the remaining fixed run
    # or raw-log/profile preservation. They do not change the recorded test's exit status.
    set +e
    "${perf_bin}" report -i "${out}/${state}.data" --stdio --no-children --percent-limit 0.5 \
        > "${out}/${state}-report.txt" 2> "${out}/${state}-report-errors.txt"
    report_rc=$?
    "${perf_bin}" script -i "${out}/${state}.data" --ns \
        -F comm,pid,tid,time,event,ip,sym,dso \
        2> "${out}/${state}-script-errors.txt" | gzip > "${out}/${state}-stacks.txt.gz"
    script_rc=$?
    set -e
    printf '%s report_exit=%s script_exit=%s\n' "${state}" "${report_rc}" "${script_rc}" \
        | tee -a "${out}/results.txt"
    [ "${report_rc}" -eq 0 ] && [ "${script_rc}" -eq 0 ] || status=1
    find "${MESA_SHADER_CACHE_DIR}" -type f -printf '%s %p\n' \
        > "${out}/${state}-cache-after.txt"
done
exit "${status}"
