#!/usr/bin/env bash
# The codegen runner. Four checks, none of which needs the runtime: a stub
# cc stands in for the real one, since what is under test is the text geas
# emits, the module C, the entry wrapper, the header, the .proto, and the
# two session wrappers.
#
# The goldens in compiler/tests/codegen pin that text for a few
# representative programs, six from skeleton and the ones in
# compiler/tests/codegen/src, which cover what the skeleton leaves thin, the
# build errors included. Beside the files each command wrote sits a .log,
# its exit code over its stderr. The workflow goldens in
# compiler/tests/codegen/workflow pin the host driven workflow surface the
# same way, the order saga and the flowdesk contracts from skeleton and the
# workflow programs of tests/check that build. Every program is emitted
# twice and the two runs are held to each other, so the text is a function
# of the source and nothing else. The coverage check walks the accepted
# programs of tests/check, each one has to be a workflow golden, on the
# refused list, or on the plain list, so a new one cannot slip between
# them. The refusals hold every workflow program that reaches for the
# engine driven half, a course, amends, await, select, sleep, together, or
# now(), to the named build error.
#
# Set GEAS to test another build, the sanitizer one at target/bin-asan/geas
# for instance. BLESS=1 rewrites every golden from GEAS and does nothing
# else, read the diff before keeping it

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

GEAS="${GEAS:-$ROOT/target/bin/geas}"
GOLDEN_DIR="$ROOT/compiler/tests/codegen"
GOLDEN_SOURCES="$(printf 'skeleton/%s.geas ' hello payment ledger lang main_demo std_user)
$(cd "$ROOT" && ls compiler/tests/codegen/src/*.geas)"
OUT_DIR="target/geas-out"
WORKFLOW_DIR="$ROOT/compiler/tests/codegen/workflow"
WORKFLOW_SOURCES="skeleton/saga.geas skeleton/flowdesk.geas
$(printf 'tests/check/%s.geas ' remedy_ok signal_ok term_attr_ok terms_ok)"

# The workflow programs of tests/check the build refuses, each reaching for
# the engine driven half of the surface
REFUSED_SOURCES="amends_ok course_ok order_saga time_ok"

# The accepted programs of tests/check with no workflow construct in them,
# which pin nothing the goldens above do not
PLAIN_SOURCES="abi_clause_ok incorporate misc payment requirements std_user
store_ledger sub_transactional_ok attribute_name sizes req_forty req_worst_case bytes bytes_shadow process"

if [ ! -x "$GEAS" ]; then
    echo "[codegen] no geas binary at $GEAS, run make compiler first" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/geas-codegen.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# The stub cc: links nothing and succeeds, so a build runs to its last line
mkdir -p "$WORK/bin"
printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/cc"
chmod +x "$WORK/bin/cc"

pass=0
fail=0

fail_case() {
    echo "FAIL $1: $2"
    fail=$((fail + 1))
}

# Runs one geas command in a scratch directory of its own, where lib points
# at the repository's library so imports resolve the way they do at the
# root. Leaves the exit code in $dir/code, stderr in $dir/stderr, and the
# two as one text in $dir/log with the repository path cut from every line
run_in() {
    local dir="$1" bin="$2"
    shift 2
    mkdir -p "$dir"
    [ -e "$dir/lib" ] || ln -s "$ROOT/lib" "$dir/lib"
    (cd "$dir" && PATH="$WORK/bin:$PATH" GEAS_ROOT="$ROOT" "$bin" "$@" >"$dir/stdout" 2>"$dir/stderr")
    echo $? > "$dir/code"
    { echo "exit $(cat "$dir/code")"; sed "s|$ROOT/||g" "$dir/stderr"; } > "$dir/log"
}

# Every artifact of one program from one binary, into $dir/target/geas-out
emit_all() {
    local dir="$1" bin="$2" src="$3"
    run_in "$dir/build" "$bin" build "$src"
    run_in "$dir/bin" "$bin" build --bin "$src"
    run_in "$dir/header" "$bin" emit-header "$src"
    run_in "$dir/proto" "$bin" emit-proto "$src"
}

# The golden name of an emitted file: the --bin module C is kept apart from
# the plain one, though the two are the same text
golden_name() {
    local mode="$1" file="$2"
    if [ "$mode" = "bin" ] && [ "${file%.main.c}" = "$file" ]; then
        echo "${file%.c}.bin.c"
    else
        echo "$file"
    fi
}

# Lists what one command left behind as golden name and path pairs: its log,
# then each file it wrote
golden_files() {
    local dir="$1" stem="$2" mode="$3" file
    echo "$stem.$mode.log $dir/$mode/log"
    for file in "$dir/$mode/$OUT_DIR"/*; do
        [ -f "$file" ] || continue
        echo "$(golden_name "$mode" "$(basename "$file")") $file"
    done
}

# Rewrites one golden directory from the build under test
bless_dir() {
    local sources="$1" golden_dir="$2" src stem mode name file
    mkdir -p "$golden_dir"
    find "$golden_dir" -maxdepth 1 -type f -delete
    for src in $sources; do
        stem="$(basename "${src%.geas}")"
        emit_all "$WORK/bless/$stem" "$GEAS" "$ROOT/$src"
        for mode in build bin header proto; do
            while read -r name file; do
                cp "$file" "$golden_dir/$name"
            done < <(golden_files "$WORK/bless/$stem" "$stem" "$mode")
        done
    done
    echo "[codegen] blessed $(find "$golden_dir" -maxdepth 1 -type f | wc -l) goldens in ${golden_dir#"$ROOT"/} from $GEAS"
}

# Holds the build to the goldens, then emits a second time and holds the
# two runs to each other. Every golden has to be produced by some command
check_goldens() {
    local sources="$1" golden_dir="$2" src stem mode name file other
    : > "$WORK/golden/produced.$(basename "$golden_dir")"
    for src in $sources; do
        stem="$(basename "${src%.geas}")"
        emit_all "$WORK/golden/$stem" "$GEAS" "$ROOT/$src"
        emit_all "$WORK/again/$stem" "$GEAS" "$ROOT/$src"
        for mode in build bin header proto; do
            while read -r name file; do
                other="$WORK/again/$stem/${file#"$WORK/golden/$stem/"}"
                echo "$name" >> "$WORK/golden/produced.$(basename "$golden_dir")"
                if ! cmp -s "$file" "$golden_dir/$name"; then
                    fail_case "golden/$name" "differs from the golden"
                    diff "$golden_dir/$name" "$file" 2>&1 | head -10
                elif ! cmp -s "$file" "$other"; then
                    fail_case "golden/$name" "two runs over one source differ"
                else
                    pass=$((pass + 1))
                fi
            done < <(golden_files "$WORK/golden/$stem" "$stem" "$mode")
        done
    done
    for file in "$golden_dir"/*; do
        [ -f "$file" ] || continue
        name="$(basename "$file")"
        if ! grep -qxF "$name" "$WORK/golden/produced.$(basename "$golden_dir")"; then
            fail_case "golden/$name" "the build did not emit it"
        fi
    done
}

# Every accepted program of tests/check is a workflow golden, refused, or
# plain, so a new one cannot slip between the three
check_workflow_coverage() {
    local golden stem
    for golden in tests/check/*.expect; do
        stem="$(basename "${golden%.expect}")"
        case " $(echo $REFUSED_SOURCES $PLAIN_SOURCES) " in *" $stem "*) continue ;; esac
        if printf '%s\n' $WORKFLOW_SOURCES | grep -qxF "tests/check/$stem.geas"; then
            pass=$((pass + 1))
        else
            fail_case "workflow/$stem" "accepted, neither a workflow golden, refused, nor plain"
        fi
    done
}

# Every program on the refused list has to be refused by each artifact
# command with the named build error, and must write nothing
check_refusals() {
    local stem src base cmd dir
    for stem in $REFUSED_SOURCES; do
        src="$ROOT/tests/check/$stem.geas"
        base="refuse/$stem"
        for cmd in build emit-header emit-proto; do
            dir="$WORK/$base/$cmd"
            run_in "$dir" "$GEAS" "$cmd" "$src"
            if [ "$(cat "$dir/code")" -ne 1 ]; then
                fail_case "$base" "$cmd expected exit 1, got $(cat "$dir/code")"
            elif ! grep -q "geas: workflow constructs are not emitted yet: " "$dir/stderr"; then
                fail_case "$base" "$cmd stderr names no workflow construct"
            elif [ -e "$dir/$OUT_DIR" ]; then
                fail_case "$base" "$cmd wrote files for a refused program"
            else
                pass=$((pass + 1))
            fi
        done
    done
}

if [ "${BLESS:-0}" = "1" ]; then
    bless_dir "$GOLDEN_SOURCES" "$GOLDEN_DIR"
    bless_dir "$WORKFLOW_SOURCES" "$WORKFLOW_DIR"
    exit 0
fi

mkdir -p "$WORK/golden"
check_goldens "$GOLDEN_SOURCES" "$GOLDEN_DIR"
check_goldens "$WORKFLOW_SOURCES" "$WORKFLOW_DIR"
check_workflow_coverage
check_refusals

echo "[codegen] $pass passed, $fail failed"
[ "$fail" -eq 0 ]
