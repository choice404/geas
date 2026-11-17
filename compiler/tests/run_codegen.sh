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
# workflow programs of tests/check that build. The course goldens in
# compiler/tests/codegen/course pin the engine driven half, the course
# function and the descriptor that carries it, over representative courses
# of tests/course, the order saga with a course, and the course programs of
# tests/check. The amends goldens in compiler/tests/codegen/amends pin the
# amends tables and their vows functions, over the programs in
# compiler/tests/codegen/amends/src, whose recorded shapes sit in its
# shapes/, and the accepted amends programs of tests/check. Every program is
# emitted twice and the two runs are held to each other, so the text is a
# function of the source and nothing else. The coverage check walks the
# accepted programs of tests/check, each one has to be a workflow, course,
# or amends golden or on the plain list, so a new one cannot slip between
# them. The syntax check runs the real cc over the module C of every golden
# and every course of tests/course: it has to compile under -Wall -Wextra,
# no warning may land inside a course function or a type table, and with
# gcc no goto may jump over an initialization. Module C that carries an
# amends table compiles only against a runtime header that declares
# GeasAmendDesc, and module C that carries a type table only against one
# that declares GeasTypeNode; against a header without them such a file is
# counted as skipped
# The tree goldens pin every remaining program under skeleton, lib/std, and
# examples, including command failures from library modules and partial
# skeletons
#
# Set GEAS to test another build, the sanitizer one at target/bin-asan/geas
# for instance. BLESS=1 rewrites every golden from GEAS and does nothing
# else, read the diff before keeping it

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

GEAS="${GEAS:-$ROOT/target/bin/geas}"
GOLDEN_DIR="$ROOT/compiler/tests/codegen"
GOLDEN_SOURCES="$(printf 'skeleton/%s.geas ' hello payment ledger lang main_demo std_user boundary_host)
$(cd "$ROOT" && ls compiler/tests/codegen/src/*.geas)"
OUT_DIR="target/geas-out"
WORKFLOW_DIR="$ROOT/compiler/tests/codegen/workflow"
WORKFLOW_SOURCES="skeleton/saga.geas skeleton/flowdesk.geas skeleton/boundary.geas
$(printf 'tests/check/%s.geas ' remedy_ok signal_ok term_attr_ok terms_ok)"
COURSE_DIR="$ROOT/compiler/tests/codegen/course"
COURSE_SOURCES="$(printf 'tests/course/%s.geas ' straight_line while_head for_list if_match select_arms together questions)
$(cd "$ROOT" && ls compiler/tests/codegen/course/src/*.geas)
$(printf 'tests/check/%s.geas ' course_ok time_ok course_nested_ok course_together_ok course_local_ok course_body_context_ok)"

# The course of tests/course whose build stops before any module C: its
# record vow is outside what a compiled descriptor carries, so the syntax
# check holds its build to that one fault instead
UNBUILT_COURSES="order_saga"

AMENDS_DIR="$ROOT/compiler/tests/codegen/amends"
AMENDS_SOURCES="$(cd "$ROOT" && ls compiler/tests/codegen/amends/src/*.geas)
$(printf 'tests/check/%s.geas ' amends_ok order_saga amends_vows_ok amends_items_ok amends_parts_ok amends_course_ok \
    amends_layout_ok amends_vow_order_ok amends_seed_remedy_ok)"
TREE_DIR="$ROOT/compiler/tests/codegen/tree"
TREE_SOURCES="$(printf 'skeleton/%s.geas ' blob hello_v2 jobattrs jobstep lifecycle order_course order_course_v2 steps)
$(printf 'lib/std/%s.geas ' collections errors math process store strings traits)
examples/supervisor/service.geas"

# The tree modules whose generated C has to carry a course or amends table
TREE_SYNTAX_STEMS="order_course order_course_v2"

# The accepted programs of tests/check with no workflow construct in them,
# which pin nothing the goldens above do not
PLAIN_SOURCES="abi_clause_ok abi_pledge_type_ok incorporate misc payment requirements std_user
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
skipped=0
HAS_AMEND_ABI=0
if grep -q "GeasAmendDesc" "$ROOT/runtime/include/geas/geas_abi.h" 2>/dev/null; then
    HAS_AMEND_ABI=1
fi
HAS_TYPE_ABI=0
if grep -q "GeasTypeNode" "$ROOT/runtime/include/geas/geas_abi.h" 2>/dev/null; then
    HAS_TYPE_ABI=1
fi

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

# Every accepted program of tests/check is a workflow, course, or amends
# golden, or plain, so a new one cannot slip between them
check_workflow_coverage() {
    local golden stem
    for golden in tests/check/*.expect; do
        stem="$(basename "${golden%.expect}")"
        case " $(echo $PLAIN_SOURCES) " in *" $stem "*) continue ;; esac
        if printf '%s\n' $WORKFLOW_SOURCES $COURSE_SOURCES $AMENDS_SOURCES |
            grep -qxF "tests/check/$stem.geas"; then
            pass=$((pass + 1))
        else
            fail_case "workflow/$stem" "accepted, neither a workflow, course, or amends golden, nor plain"
        fi
    done
}

# Every source in the public source trees has to belong to a golden table
check_source_tree_coverage() {
    local src
    while read -r src; do
        if printf '%s\n' $GOLDEN_SOURCES $WORKFLOW_SOURCES $COURSE_SOURCES \
            $AMENDS_SOURCES $TREE_SOURCES | grep -qxF "$src"; then
            pass=$((pass + 1))
        else
            fail_case "coverage/$src" "source is not listed in a golden table"
        fi
    done < <(find skeleton lib/std examples -type f -name '*.geas' | sort)
}

# The line ranges of the course functions of one C file, as first last pairs
course_ranges() {
    awk '/^static uint32_t k[0-9]+_[A-Za-z0-9_]*_course\(/ { first = NR }
         first && /^}$/ { print first, NR; first = 0 }' "$1"
}

# The line ranges of the type table arrays of one C file, as first last
# pairs: a one line index array, or a table that runs to its closing brace
type_ranges() {
    awk '/^static const (uint32_t|GeasTypeVariant|GeasTypeNode|GeasItemTypes) k[0-9]+_[A-Za-z0-9_]*_(type_kids_|type_fields_|type_variants_|types\[|item_params_|item_types\[|vow_types\[|remedy_params_|remedy_types\[)/ {
             if ($0 ~ /;$/) { print NR, NR } else { first = NR }
             next
         }
         first && /^};$/ { print first, NR; first = 0 }' "$1"
}

# Whether a line of a C file falls inside one of the ranges a lister prints
in_ranges() {
    local lister="$1" file="$2" number="$3" first last
    while read -r first last; do
        if [ "$number" -ge "$first" ] && [ "$number" -le "$last" ]; then
            return 0
        fi
    done < <("$lister" "$file")
    return 1
}

# Runs the real cc over one module C file: a failure, a warning inside a
# course function or a type table, or a goto over an initialization fails
# it. A file that carries an amends table or a type table is skipped when
# the runtime header has no GeasAmendDesc or GeasTypeNode to compile it
# against
check_c_file() {
    local name="$1" file="$2" log="$WORK/syntax.log" number
    if [ "$HAS_AMEND_ABI" -eq 0 ] && grep -q "GeasAmendDesc" "$file"; then
        skipped=$((skipped + 1))
        return
    fi
    if [ "$HAS_TYPE_ABI" -eq 0 ] && grep -q "GeasTypeNode" "$file"; then
        skipped=$((skipped + 1))
        return
    fi
    if ! $CC_CHECK "$file" > "$log" 2>&1; then
        fail_case "syntax/$name" "cc rejects the module C"
        head -10 "$log"
        return
    fi
    while IFS=: read -r _ number _; do
        if in_ranges course_ranges "$file" "$number"; then
            fail_case "syntax/$name" "a warning inside a course function at line $number"
            grep -F ":$number:" "$log" | head -3
            return
        fi
        if in_ranges type_ranges "$file" "$number"; then
            fail_case "syntax/$name" "a warning inside a type table at line $number"
            grep -F ":$number:" "$log" | head -3
            return
        fi
    done < <(grep -F ': warning: ' "$log")
    pass=$((pass + 1))
}

# The module C of every golden, the entry wrappers aside, then every course
# of tests/course, emitted fresh
check_module_syntax() {
    local cc src stem dir file
    cc="$(command -v cc || true)"
    if [ -z "$cc" ]; then
        echo "[codegen] no cc on PATH, the syntax check is skipped"
        return
    fi
    CC_CHECK="$cc -std=c11 -Wall -Wextra -fsyntax-only -I $ROOT/runtime/include"
    if "$cc" -Werror=jump-misses-init -x c -fsyntax-only - < /dev/null > /dev/null 2>&1; then
        CC_CHECK="$CC_CHECK -Werror=jump-misses-init"
    else
        echo "[codegen] cc has no -Wjump-misses-init, the goto check rides on gcc alone"
    fi
    for stem in $TREE_SYNTAX_STEMS; do
        if [ ! -f "$TREE_DIR/$stem.c" ]; then
            fail_case "syntax/tree/$stem" "the golden table has no module C"
        fi
    done
    for file in "$GOLDEN_DIR"/*.c "$WORKFLOW_DIR"/*.c "$COURSE_DIR"/*.c "$AMENDS_DIR"/*.c \
        "$TREE_DIR"/*.c; do
        [ -f "$file" ] || continue
        case "$file" in *.main.c) continue ;; esac
        check_c_file "${file#"$GOLDEN_DIR"/}" "$file"
    done
    for src in tests/course/*.geas; do
        stem="$(basename "${src%.geas}")"
        dir="$WORK/syntax/$stem"
        run_in "$dir" "$GEAS" build "$ROOT/$src"
        case " $UNBUILT_COURSES " in
            *" $stem "*)
                if grep -q "not yet supported: a vow of this type in a compiled module" "$dir/stderr"; then
                    pass=$((pass + 1))
                else
                    fail_case "syntax/$stem" "build did not stop at the record vow"
                fi
                continue
                ;;
        esac
        file="$dir/$OUT_DIR/$stem.c"
        if [ ! -f "$file" ]; then
            fail_case "syntax/$stem" "build wrote no module C"
            continue
        fi
        check_c_file "$stem" "$file"
    done
    if [ "$skipped" -gt 0 ]; then
        echo "[codegen] runtime/include/geas/geas_abi.h declares no GeasAmendDesc or no" \
            "GeasTypeNode, $skipped module C files that carry such a table were not compiled"
    fi
}

if [ "${BLESS:-0}" = "1" ]; then
    bless_dir "$GOLDEN_SOURCES" "$GOLDEN_DIR"
    bless_dir "$WORKFLOW_SOURCES" "$WORKFLOW_DIR"
    bless_dir "$COURSE_SOURCES" "$COURSE_DIR"
    bless_dir "$AMENDS_SOURCES" "$AMENDS_DIR"
    bless_dir "$TREE_SOURCES" "$TREE_DIR"
    exit 0
fi

mkdir -p "$WORK/golden"
check_goldens "$GOLDEN_SOURCES" "$GOLDEN_DIR"
check_goldens "$WORKFLOW_SOURCES" "$WORKFLOW_DIR"
check_goldens "$COURSE_SOURCES" "$COURSE_DIR"
check_goldens "$AMENDS_SOURCES" "$AMENDS_DIR"
check_goldens "$TREE_SOURCES" "$TREE_DIR"
check_workflow_coverage
check_source_tree_coverage
check_module_syntax

echo "[codegen] $pass passed, $fail failed"
[ "$fail" -eq 0 ]
