#!/bin/sh
# Fail when an object the build names cannot actually be built.
#
# A rule with no recipe satisfies make: it reports nothing, runs only the
# order-only mkdir, and the failure lands on the linker instead. That is how
# the posix pipe recipes went missing twice, so this walks the object lists
# and demands a real compile command for each one.
#
# The check is positive on purpose. Looking for "Nothing to be done" misses
# the silent case above; requiring a compiler invocation cannot be fooled by
# an order-only prerequisite recipe that happens to print.
#
# Usage: scripts/check-build.sh <object> [<object> ...]
set -eu

make_command="${MAKE:-make}"
status=0

for object in "$@"; do
    recipe="$("$make_command" -Bn --no-print-directory "$object" 2>&1 || true)"
    case "$recipe" in
        *" -c "*|*" -f elf64 "*)
            ;;
        *)
            echo "check-build: no recipe for $object" >&2
            status=1
            ;;
    esac
done

if [ "$status" -ne 0 ]; then
    echo "check-build: an object in the build lists cannot be built" >&2
    exit 1
fi
echo "check-build: every listed object has a recipe"
