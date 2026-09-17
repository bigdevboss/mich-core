#!/bin/sh
set -eu

image=${1:?missing image}
memory=${2:?missing memory}
profile=${3:?missing recovery profile}
runs=${4:-3}

case "$profile" in
    msi-restart|msi-circuit|msi-recovery)
        ;;
    *)
        echo "qemu-recovery-stability: unsupported profile: $profile" >&2
        exit 1
        ;;
esac

case "$runs" in
    ''|*[!0-9]*)
        echo "qemu-recovery-stability: invalid run count: $runs" >&2
        exit 1
        ;;
esac

if [ "$runs" -eq 0 ]; then
    echo "qemu-recovery-stability: run count must be positive" >&2
    exit 1
fi

workdir="$(mktemp -d)"
failure_log="${MICH_STABILITY_FAILURE_LOG:-bin/x86_64/${profile}-stability-failure.log}"
trap 'rm -rf "$workdir"' EXIT

run=1
while [ "$run" -le "$runs" ]; do
    log="$workdir/${profile}-${run}.log"
    passive_trace="$workdir/${profile}-${run}.passive.log"
    if ! MICH_QEMU_PASSIVE_TRACE="$passive_trace" \
        sh ./scripts/qemu-smoke64.sh "$image" "$memory" "$profile" \
        >"$log" 2>&1; then
        mkdir -p "$(dirname "$failure_log")"
        cp "$log" "$failure_log"
        if [ -s "$passive_trace" ]; then
            cp "$passive_trace" "${failure_log}.passive"
            echo "qemu-recovery-stability: saved ${failure_log}.passive" >&2
        fi
        cat "$log"
        echo "qemu-recovery-stability: FAIL (${profile} ${run}/${runs})" >&2
        echo "qemu-recovery-stability: saved $failure_log" >&2
        exit 1
    fi
    echo "qemu-recovery-stability: PASS (${profile} ${run}/${runs})"
    run=$((run + 1))
done

echo "qemu-recovery-stability: PASS (${profile}, ${runs} boots)"
