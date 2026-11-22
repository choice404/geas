#!/usr/bin/env bash
# The golden test runner for the geas front end. Each suite directory pairs
# with a geas command: tests/lex with `geas lex`, tests/parse with
# `geas parse`, tests/check and tests/typeck with `geas check`, and
# tests/course with `geas dump-course`, whose pairs directory holds sources
# whose course hashes have to agree or differ, and tests/shape with
# `geas dump-shape`, the debug command that prints every
# shape string, shape hash, and mangled name, while tests/shape/record
# holds `geas record` to its goldens, and tests/types with
# `geas dump-types`, the debug command that prints the type table of every
# contract. Every NAME.geas with a
# NAME.expect beside it must be accepted and its dump must match the golden
# byte for byte; check writes nothing to stdout, so its goldens are a single
# blank line guarding that silence. Every NAME.geas with a
# NAME.err beside it must be rejected: a nonzero exit and every line of the
# .err file appearing somewhere in stderr as a substring, so a diagnostic can
# gain context without breaking the suite. tests/check/mfa holds the
# multi-file programs: only files with a golden beside them are roots, the
# rest are modules the roots import. tests/bin/run holds standalone programs
# built with `geas build --bin` and run, held to their exit code and stderr.
# The geas binary is built with
# `make compiler` when missing; set GEAS to test another build, the
# sanitizer one at target/bin-asan/geas for instance. The commands of a
# golden suite run side by side, JOBS at a time and the machine's width when
# it is unset, and their results are then read in order, so the report and
# the count are the ones a run one case at a time gives.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

GEAS="${GEAS:-$ROOT/target/bin/geas}"

if [ ! -x "$GEAS" ]; then
    echo "[tests] no geas binary at $GEAS, running make compiler" >&2
    make compiler >&2 || exit 1
fi

if [ ! -x "$GEAS" ]; then
    echo "[tests] make compiler left no binary at $GEAS" >&2
    exit 1
fi

pass=0
fail=0

fail_case() {
    echo "FAIL $1: $2"
    fail=$((fail + 1))
}

# How many cases run at once, and the scratch directory their verdicts land
# in
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
VERDICTS="$(mktemp -d "${TMPDIR:-/tmp}/geas-tests.XXXXXX")"
trap 'rm -rf "$VERDICTS"' EXIT

# Runs a command in the background, holding at most JOBS of them at once.
# running counts what was spawned since the last full wait, which settle
# takes and resets
running=0
spawn() {
    if [ "$running" -ge "$JOBS" ]; then
        wait -n
        running=$((running - 1))
    fi
    "$@" &
    running=$((running + 1))
}

settle() {
    wait
    running=0
}

# One golden case: an accepted program held to its dump, a rejected one to
# the diagnostics its .err lists. Prints what a failure reports and answers
# 0 for a pass, 1 for a failure, and 2 for a file that is no case, a module
# of the multi-file suite with no golden beside it, or for a file with no
# golden in the other suites a failure of its own
judge_case() {
    local cmd="$1" src="$2" base="$3" is_multi="$4"
    local name out code err missing want
    name="${src%.geas}"
    if [ -f "$name.expect" ]; then
        out="$("$GEAS" "$cmd" "$src" 2>/dev/null)"
        code=$?
        if [ "$code" -ne 0 ]; then
            echo "FAIL $base: expected exit 0, got $code"
            return 1
        fi
        if ! diff <(printf '%s\n' "$out") "$name.expect" >/dev/null; then
            echo "FAIL $base: dump differs from golden"
            diff <(printf '%s\n' "$out") "$name.expect" | head -20
            return 1
        fi
        return 0
    elif [ -f "$name.err" ]; then
        err="$("$GEAS" "$cmd" "$src" 2>&1 >/dev/null)"
        code=$?
        if [ "$code" -eq 0 ]; then
            echo "FAIL $base: expected rejection, got exit 0"
            return 1
        fi
        missing=""
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            if ! printf '%s' "$err" | grep -qF -- "$want"; then
                missing="$want"
                break
            fi
        done < "$name.err"
        if [ -n "$missing" ]; then
            echo "FAIL $base: stderr missing: $missing"
            printf '%s\n' "$err" | head -10
            return 1
        fi
        return 0
    elif [ "$is_multi" -eq 1 ]; then
        return 2
    fi
    echo "FAIL $base: no .expect or .err beside it"
    return 1
}

# One case in the background, its report and its verdict kept under stem
judge_into() {
    local stem="$1"
    shift
    judge_case "$@" > "$stem.log" 2>&1
    echo $? > "$stem.verdict"
}

# Every case of a suite directory judged side by side, then read back in
# order, so the report and the counts come out as one case at a time gives
judge_dir() {
    local dir="$1" cmd="$2" is_multi="$3" src stem index=0 verdict
    mkdir -p "$VERDICTS/$dir"
    for src in tests/"$dir"/*.geas; do
        index=$((index + 1))
        spawn judge_into "$VERDICTS/$dir/$index" "$cmd" "$src" \
            "$dir/$(basename "${src%.geas}")" "$is_multi"
    done
    settle
    index=0
    for src in tests/"$dir"/*.geas; do
        index=$((index + 1))
        stem="$VERDICTS/$dir/$index"
        cat "$stem.log"
        read -r verdict < "$stem.verdict"
        case "$verdict" in
            0) pass=$((pass + 1)) ;;
            1) fail=$((fail + 1)) ;;
        esac
    done
}

run_suite() {
    judge_dir "$1" "$2" 0
}

# The multi-file suite. Every NAME.geas in the directory with a NAME.expect
# or NAME.err beside it is a program root and runs exactly like a check
# case; a file with neither is a helper another root imports and is skipped
# rather than failed, since it is not a test by itself.
run_multi_suite() {
    judge_dir "$1" check 1
}

# The standalone reject suite. Every NAME.geas here breaks the entry point
# rule some way, so `geas build --bin` must refuse it with every line of the
# .err beside it on stderr; nothing in this directory is expected to build.
run_bin_suite() {
    local src name base code err missing want
    for src in tests/bin/*.geas; do
        name="${src%.geas}"
        base="bin/$(basename "$name")"

        if [ ! -f "$name.err" ]; then
            fail_case "$base" "no .err beside it"
            continue
        fi
        err="$("$GEAS" build --bin "$src" 2>&1 >/dev/null)"
        code=$?
        if [ "$code" -eq 0 ]; then
            fail_case "$base" "expected rejection, got exit 0"
            continue
        fi
        missing=""
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            if ! printf '%s' "$err" | grep -qF -- "$want"; then
                missing="$want"
                break
            fi
        done < "$name.err"
        if [ -n "$missing" ]; then
            fail_case "$base" "stderr missing: $missing"
            printf '%s\n' "$err" | head -10
            continue
        fi
        pass=$((pass + 1))
    done
}

# The run suite. Every NAME.geas in tests/bin/run builds with
# `geas build --bin` in a scratch directory of its own and runs once per
# line of NAME.run beside it. A line is the exit code, a text stderr has to
# hold or nothing, and the arguments, split on '|'; a line opening with '#'
# is a comment. The suite links against the runtime library, so it is
# skipped when target/geas-out/libgeasrt.so or cc is missing
RUNTIME_LIBRARY="target/geas-out/libgeasrt.so"

# One line of a .run file against the built program
run_exec_case() {
    local exe="$1" base="$2" code="$3" want="$4" args="$5" err status
    # shellcheck disable=SC2086
    err="$("$exe" $args 2>&1 >/dev/null)"
    status=$?
    if [ "$status" -ne "$code" ]; then
        fail_case "$base [$args]" "expected exit $code, got $status"
    elif [ -n "$want" ] && ! printf '%s' "$err" | grep -qF -- "$want"; then
        fail_case "$base [$args]" "stderr missing: $want"
    else
        pass=$((pass + 1))
    fi
}

run_exec_suite() {
    local work src name base code want args
    if [ ! -f "$RUNTIME_LIBRARY" ] || ! command -v cc >/dev/null 2>&1; then
        echo "[tests] no $RUNTIME_LIBRARY or no cc, the run suite is skipped"
        return
    fi
    work="$(mktemp -d "${TMPDIR:-/tmp}/geas-run.XXXXXX")"
    for src in tests/bin/run/*.geas; do
        name="$(basename "${src%.geas}")"
        base="bin/run/$name"
        mkdir -p "$work/$name"
        if ! (cd "$work/$name" && GEAS_ROOT="$ROOT" "$GEAS" build --bin "$ROOT/$src" >/dev/null 2>&1); then
            fail_case "$base" "build --bin failed"
            continue
        fi
        while IFS='|' read -r code want args; do
            case "$code" in ''|'#'*) continue ;; esac
            run_exec_case "$work/$name/target/geas-out/$name" "$base" "$code" "$want" "$args"
        done < "${src%.geas}.run"
    done
    rm -rf "$work"
}

# The record suite. Every NAME.geas in tests/shape/record is an older
# version an amends case is checked against. Each is recorded in a scratch
# directory of its own, and every file the record writes must match the
# golden of the same name in tests/shape/record/shapes byte for byte. A
# second record must leave the files as they are, a recorded file edited
# since must be refused with nothing written, and --force must write the
# golden back. Then every recorded shape a check, course, or codegen case
# reads has to be one of those goldens, but the hand made ones that hold a
# broken file on purpose
RECORD_GOLDENS="tests/shape/record/shapes"
HANDMADE_SHAPES="AmendCorrupt.v1.shape AmendFormat.v1.shape AmendMismatch.v1.shape"
HANDMADE_SHAPES="$HANDMADE_SHAPES AmendPrior.v1.shape AmendRetyped.v1.shape AmendStale.v1.shape"

# Whether every file a record wrote into dir matches its golden
matches_goldens() {
    local dir="$1" file golden
    for file in "$dir"/shapes/*.shape; do
        golden="$RECORD_GOLDENS/$(basename "$file")"
        if [ ! -f "$golden" ] || ! cmp -s "$file" "$golden"; then
            echo "$(basename "$file") differs from $golden"
            return 1
        fi
    done
}

# A second record keeps the files; one edited since is refused, then forced back
record_again() {
    local dir="$1" src="$2" base="$3" file err
    err="$("$GEAS" record "$src" 2>&1)" || { fail_case "$base" "second record failed"; return; }
    if ! printf '%s' "$err" | grep -qF "already records this shape" || ! matches_goldens "$dir"; then
        fail_case "$base" "second record was not a no op"
        return
    fi
    file="$(ls "$dir"/shapes/*.shape | head -1)"
    printf 'edited\n' >> "$file"
    if err="$("$GEAS" record "$src" 2>&1)" ||
        ! printf '%s' "$err" | grep -qF "already records a different shape"; then
        fail_case "$base" "an edited shape was not refused"
        return
    fi
    if ! "$GEAS" record "$src" --force 2>/dev/null || ! matches_goldens "$dir"; then
        fail_case "$base" "record --force did not write the golden back"
        return
    fi
    pass=$((pass + 1))
}

run_record_suite() {
    local work src name dir
    work="$(mktemp -d "${TMPDIR:-/tmp}/geas-record.XXXXXX")"
    for src in tests/shape/record/*.geas; do
        name="$(basename "${src%.geas}")"
        dir="$work/$name"
        mkdir -p "$dir"
        cp "$src" "$dir/"
        if ! "$GEAS" record "$dir/$name.geas" 2>/dev/null; then
            fail_case "record/$name" "record failed"
        elif ! matches_goldens "$dir"; then
            fail_case "record/$name" "a recorded shape differs from its golden"
        else
            pass=$((pass + 1))
            record_again "$dir" "$dir/$name.geas" "record/$name"
        fi
    done
    rm -rf "$work"
}

# Every recorded shape a case reads is a golden the record suite produces
check_shape_fixtures() {
    local file name
    while IFS= read -r file; do
        name="$(basename "$file")"
        case " $HANDMADE_SHAPES " in *" $name "*) continue ;; esac
        if [ -f "$RECORD_GOLDENS/$name" ] && cmp -s "$file" "$RECORD_GOLDENS/$name"; then
            pass=$((pass + 1))
        else
            fail_case "fixture/$file" "not the shape geas record writes for it"
        fi
    done < <(find tests/check tests/course compiler/tests/codegen -name '*.shape' | sort)
}

# The pairs of tests/shape: NAME_before.geas and NAME_after.geas differ only
# in one thing the shape covers, the layout of a declared type the boundary
# reaches, the members of a subcontract, its transactional modifier, or a
# requirements block, so their shape hashes have to differ, while their
# goldens pin the rest
run_layout_pairs() {
    local before after base hashBefore hashAfter
    for before in tests/shape/*_before.geas; do
        after="${before%_before.geas}_after.geas"
        base="shape/$(basename "${before%_before.geas}") pair"
        if [ ! -f "$after" ]; then
            fail_case "$base" "no $(basename "$after") beside it"
            continue
        fi
        hashBefore="$("$GEAS" dump-shape "$before" 2>/dev/null | grep '^#define GEAS_HASH_')"
        hashAfter="$("$GEAS" dump-shape "$after" 2>/dev/null | grep '^#define GEAS_HASH_')"
        if [ -z "$hashBefore" ] || [ "$hashBefore" = "$hashAfter" ]; then
            fail_case "$base" "the change left the shape hash where it was"
            continue
        fi
        pass=$((pass + 1))
    done
}

# The course hash of every contract a dump-course prints, one per line
course_hashes() {
    "$GEAS" dump-course "$1" 2>/dev/null | grep -E '^ +hash: 0x'
}

# The pairs of tests/course/pairs: NAME_same_a.geas and NAME_same_b.geas
# differ in nothing the course runs, comments, spacing, a local's or a
# binding's name, the order of a record literal's fields, a literal's
# spelling, a pledge the course never names, so their course hashes have to agree;
# NAME_differ_a.geas and NAME_differ_b.geas differ in one thing it does, so
# theirs have to differ
run_course_pairs() {
    local first second base hashFirst hashSecond
    for first in tests/course/pairs/*_a.geas; do
        second="${first%_a.geas}_b.geas"
        base="course/pairs/$(basename "${first%_a.geas}")"
        hashFirst="$(course_hashes "$first")"
        hashSecond="$(course_hashes "$second")"
        if [ ! -f "$second" ] || [ -z "$hashFirst" ] || [ -z "$hashSecond" ]; then
            fail_case "$base" "both halves have to dump a course"
            continue
        fi
        case "$base" in
            *_same)
                if [ "$hashFirst" != "$hashSecond" ]; then
                    fail_case "$base" "the course hashes differ"
                    continue
                fi
                ;;
            *_differ)
                if [ "$hashFirst" = "$hashSecond" ]; then
                    fail_case "$base" "the course hashes agree"
                    continue
                fi
                ;;
            *)
                fail_case "$base" "a pair is named NAME_same or NAME_differ"
                continue
                ;;
        esac
        pass=$((pass + 1))
    done
}

# Cross check every small requirements tree, including rejected programs
if "$GEAS" test-requirements tests/check/*.geas tests/typeck/*.geas tests/parse/*.geas \
    tests/shape/*.geas compiler/tests/codegen/src/*.geas skeleton/*.geas; then
    pass=$((pass + 1))
else
    fail_case requirements/self-test "solver cross check failed"
fi

run_suite lex lex
run_suite parse parse
run_suite check check
run_suite typeck check
run_suite shape dump-shape
run_layout_pairs
run_suite course dump-course
run_course_pairs
run_suite types dump-types
run_multi_suite check/mfa
run_bin_suite
run_exec_suite
run_record_suite
check_shape_fixtures

echo "[tests] $pass passed, $fail failed"
[ "$fail" -eq 0 ]
