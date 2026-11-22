# The geas build. Five artifacts: the geas binary, a C++23 compiler CMake
# builds from compiler/ into target/bin, the runtime shared library, the
# compiled hello and hello_v2 modules, and the C host that drives them. make
# smoke runs the whole pipeline and is the walking skeleton gate every
# milestone keeps green. The runtime is threaded, so every gate builds with
# -pthread and the tsan gate reruns the concurrency surface under
# ThreadSanitizer; test-iname gates the discovery table and the freeze, and
# test-determinism proves two builds of one source emit byte identical
# module C.
#
# GEAS points at the compiler every gate runs, target/bin/geas unless told
# otherwise. make compiler SANITIZE=ON builds the same sources under ASan and
# UBSan into target/bin-asan, and GEAS=target/bin-asan/geas runs any gate
# with that binary.
#
# The gates come in four tiers. make quick is the front end goldens, the
# codegen goldens, the smoke walk, and two runtime gates, about fifteen
# seconds serial, and the one to run while changing code. make check runs every test exactly
# once, each TSan gate one round and each fuzz pass a short count, and is
# the daily full run; make all is make check. Every gate is safe to run
# beside every other one, so -j is the way to run it, make -j24 check takes
# about fifteen seconds here with the gates built, twenty five from nothing
# but the compiler, and a plain serial make check a little over two minutes. make soak is the long
# tail, the repeated TSan rounds, the full fuzz counts, and the hundred
# thousand round reuse case, every round of every test its own make target
# so the rounds run side by side; it runs at the machine's width even when
# make itself was not given -j, takes about a minute here, and SOAK_ROUNDS
# sets the rounds of every repeated test at once. make check-net is the
# gRPC bridge and supervisor gates, which need grpcio, Go, and Node, and stay
# out of check the way they always have.
#
# Every gate name still runs on its own, make test-saga or make tsan-workflow
# for instance, and runs the same one round check runs.

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -fPIC
OUT     := target/geas-out

# Python leaves no bytecode in the tree, since gates run it side by side
export PYTHONDONTWRITEBYTECODE := 1

# The runtime translation units a standalone gate links into its own binary,
# the shared library split into sources: the runtime, its wire codec, and the
# store driver. Kept as one list so a gate that links the runtime in gets the
# wire codec and the store driver with it. The workflow units ride the same
# list: terms.c carries the clock, ordering, deadlines, and retries, remedy.c
# the remedy pass, signal.c signal delivery, and flowpark.c the workflow blob
# of the park row, all over the structs runtime_internal.h shares out.
RT_UNITS := runtime/src/runtime.c runtime/src/wire.c runtime/src/store.c \
    runtime/src/events.c runtime/src/terms.c runtime/src/remedy.c runtime/src/signal.c \
    runtime/src/flowpark.c runtime/src/attrs.c runtime/src/bytes.c runtime/src/process.c \
    runtime/src/course.c runtime/src/coursejoin.c runtime/src/coursepark.c \
    runtime/src/amendrow.c runtime/src/amend.c runtime/src/handoff.c runtime/src/conform.c \
    runtime/src/courseslot.c runtime/src/foreign.c
RT_HEADERS := runtime/src/runtime_internal.h runtime/include/geas/geas.h \
    runtime/include/geas/geas_abi.h runtime/include/geas/geas_wire.h \
    runtime/include/geas/geas_store.h

# The runtime speaks to the store, so runtime.c references the driver in
# store.c and its vendored SQLite; every gate that links the runtime in gets
# both. RT_INC adds the amalgamation's header to the include path store.c needs.
RT_INC := -I runtime/include -I runtime/third_party

# The SQLite amalgamation, compiled once into its own object and linked into
# the runtime and every gate. It builds with warnings off and no sanitizer:
# it is a vendored library, not runtime code under review, and ASan and TSan
# tolerate an uninstrumented object linked beside instrumented ones the same
# way they tolerate a dlopened module. DQS=0 refuses double quoted string
# literals, OMIT_LOAD_EXTENSION shuts the extension door, and MEMSTATUS=0
# drops the allocation bookkeeping the store never reads.
SQLITE_OBJ := $(OUT)/sqlite3.o
SQLITE_FLAGS := -fPIC -w -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0

RT_SONAME  := libgeasrt.so.0
RT_VERSIONED := $(OUT)/$(RT_SONAME)
RT_SO      := $(OUT)/libgeasrt.so
GEAS       ?= target/bin/geas
MODULE     := $(OUT)/libhello.geas.so
MODULE_V2  := $(OUT)/libhello_v2.geas.so
MODULE_PAY := $(OUT)/libpayment.geas.so
MODULE_LANG := $(OUT)/liblang.geas.so
MODULE_STD := $(OUT)/libstd_user.geas.so
MODULE_LEDGER := $(OUT)/libledger.geas.so
MODULE_LIFE := $(OUT)/liblifecycle.geas.so
MODULE_SVC := $(OUT)/libservice.geas.so
HOST       := $(OUT)/host
BIN_DEMO   := $(OUT)/main_demo

# The compiler as a path that still names it from another directory, for the
# gates that run it inside a scratch directory of their own
GEAS_ABS = $(if $(findstring /,$(GEAS)),$(abspath $(GEAS)),$(GEAS))

# ---- the runtime, one build per flavour ----

# Every gate links the runtime in, and each flavour of it is compiled once
# into target/geas-out/rt/<flavour>/, one object per unit and a static
# archive over them, which a gate links whole so every geas_* symbol a
# dlopened module resolves against is in the binary the way it was when the
# units were compiled straight in. A gate compiles only its own test file
# with the flavour's flags and links the archive. plain is the shared
# library's own build. asan is ASan and LSan, asan-ub adds UBSan with no
# recovery, the flag set the workflow gates share, and tsan is
# ThreadSanitizer. The flavours past those three compile the runtime with a
# define a gate needs inside the runtime itself: events builds the event
# allocator's failure counter in, ckpt renames the course checkpoint so the
# saga gates can stand in front of it, and registrar renames the registrar so
# the boundary gate can count the compiled bodies. A link flag such as a
# --wrap needs no flavour of its own, since it rewrites the archive's
# references as it links them.
RT_DIR := $(OUT)/rt
RT_STEMS := $(basename $(notdir $(RT_UNITS)))
WF_SAN := -fsanitize=address,leak,undefined -fno-sanitize-recover=undefined
ASAN_SAN := -fsanitize=address,leak
TSAN_SAN := -fsanitize=thread
COURSE_SAGA_WRAP := -Dgeas_course_checkpoint=geas_course_checkpoint_real
BOUNDARY_WRAP := -Dgeas_register_contract_sized=geas_register_contract_sized_real
EVENTS_TESTING := -DGEAS_RUNTIME_TESTING

RT_FLAGS_plain := -fPIC -I runtime/src
RT_FLAGS_asan := $(ASAN_SAN) -g
RT_FLAGS_asan-ub := $(WF_SAN) -g
RT_FLAGS_asan-ub-events := $(WF_SAN) $(EVENTS_TESTING) -g
RT_FLAGS_asan-ub-ckpt := $(WF_SAN) $(COURSE_SAGA_WRAP) -g
RT_FLAGS_asan-ub-registrar := $(WF_SAN) $(BOUNDARY_WRAP) -g
RT_FLAGS_tsan := $(TSAN_SAN) -g
RT_FLAGS_tsan-events := $(TSAN_SAN) $(EVENTS_TESTING) -g
RT_FLAGS_tsan-ckpt := $(TSAN_SAN) $(COURSE_SAGA_WRAP) -g
RT_FLAVOURS := plain asan asan-ub asan-ub-events asan-ub-ckpt asan-ub-registrar \
    tsan tsan-events tsan-ckpt

rt_objs = $(addprefix $(RT_DIR)/$(1)/,$(addsuffix .o,$(RT_STEMS)))
rt_archive = $(RT_DIR)/$(1)/libgeasrt_$(1).a
rt_link = -Wl,--whole-archive $(call rt_archive,$(1)) -Wl,--no-whole-archive $(SQLITE_OBJ)

# One flavour's objects, each with the headers it read recorded beside it,
# and the archive over them, written aside and moved in so a reader never
# sees half of one
define rt_flavour
$(RT_DIR)/$(1)/%.o: runtime/src/%.c
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(RT_FLAGS_$(1)) -pthread $$(RT_INC) -MMD -MP -c $$< -o $$@

$(call rt_archive,$(1)): $(call rt_objs,$(1))
	rm -f $$@.tmp
	ar rcs $$@.tmp $$^
	mv $$@.tmp $$@
endef
$(foreach flavour,$(RT_FLAVOURS),$(eval $(call rt_flavour,$(flavour))))

runtime: $(RT_SO)

$(SQLITE_OBJ): runtime/third_party/sqlite3.c runtime/third_party/sqlite3.h
	@mkdir -p $(OUT)
	$(CC) $(SQLITE_FLAGS) -I runtime/third_party -c runtime/third_party/sqlite3.c -o $@.tmp.o
	mv $@.tmp.o $@

$(RT_SO): $(RT_VERSIONED)
	ln -sfn $(RT_SONAME) $@

$(RT_VERSIONED): $(call rt_objs,plain) $(SQLITE_OBJ)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,$(RT_SONAME) -pthread \
	    $(call rt_objs,plain) $(SQLITE_OBJ) -ldl -o $@

# ---- the gate binaries ----

# One gate binary: $(1) its name under target/geas-out, $(2) its source, $(3)
# the runtime flavour it links, $(4) its own flags, $(5) what it needs built
# first. The test compiles with the flavour's flags, so a renaming define
# reaches it the way it reaches the runtime, and the headers it read are
# recorded beside it.
define rt_test
$(OUT)/$(1): $(2) $(call rt_archive,$(3)) $(SQLITE_OBJ) $(5)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(RT_FLAGS_$(3)) -pthread $$(RT_INC) $(4) -MMD -MP -MF $$@.d -MT $$@ \
	    $(2) $$(call rt_link,$(3)) -ldl -o $$@
endef

T := tests/runtime
WRAP_MALLOC := -Wl,--wrap=malloc
WRAP_TIMEDWAIT := -Wl,--wrap=pthread_cond_timedwait
HELLO_HEADER := $(OUT)/hello.geas.h
REQ_HEADER := $(OUT)/requirements.geas.h

# Under ASan and LSan
$(eval $(call rt_test,test_value,$(T)/test_value.c,asan,$(WRAP_MALLOC)))
$(eval $(call rt_test,test_wire,$(T)/test_wire.c,asan))
$(eval $(call rt_test,test_store,$(T)/test_store.c,asan))
$(eval $(call rt_test,test_thread,$(T)/test_thread.c,asan))
$(eval $(call rt_test,test_future,$(T)/test_future.c,asan))
$(eval $(call rt_test,test_store_ledger,$(T)/test_store_ledger.c,asan,-rdynamic))
$(eval $(call rt_test,test_store_txn,$(T)/test_store_txn.c,asan,-rdynamic))
$(eval $(call rt_test,test_store_fail,$(T)/test_store_fail.c,asan,-rdynamic))
$(eval $(call rt_test,test_store_crash,$(T)/test_store_crash.c,asan,-rdynamic))
$(eval $(call rt_test,test_store_stress,$(T)/test_store_stress.c,asan,-rdynamic))
$(eval $(call rt_test,test_lifecycle,$(T)/test_lifecycle.c,asan,-rdynamic))
$(eval $(call rt_test,test_park,$(T)/test_park.c,asan,-rdynamic))
$(eval $(call rt_test,test_iname,$(T)/test_iname.c,asan,-rdynamic))
$(eval $(call rt_test,test_partial,$(T)/test_partial.c,asan,-rdynamic))
$(eval $(call rt_test,test_lang,$(T)/test_lang.c,asan,-rdynamic))
$(eval $(call rt_test,test_std,$(T)/test_std.c,asan,-rdynamic))
$(eval $(call rt_test,test_process,$(T)/test_process.c,asan,-rdynamic))
$(eval $(call rt_test,test_header,$(T)/test_header.c,asan,-rdynamic -I $(OUT),$(HELLO_HEADER)))
$(eval $(call rt_test,test_requirements,$(T)/test_requirements.c,asan,-rdynamic -I $(OUT),$(REQ_HEADER)))
$(eval $(call rt_test,host_asan,skeleton/host.c,asan,-rdynamic -I $(OUT),$(HELLO_HEADER)))

# Under ASan, LSan, and UBSan
$(eval $(call rt_test,test_terms,$(T)/test_terms.c,asan-ub))
$(eval $(call rt_test,test_remedy,$(T)/test_remedy.c,asan-ub))
$(eval $(call rt_test,test_cancel,$(T)/test_cancel.c,asan-ub))
$(eval $(call rt_test,test_handoff,$(T)/test_handoff.c,asan-ub))
$(eval $(call rt_test,test_bind_user,$(T)/test_bind_user.c,asan-ub))
$(eval $(call rt_test,test_course,$(T)/test_course.c,asan-ub))
$(eval $(call rt_test,test_course_park,$(T)/test_course_park.c,asan-ub))
$(eval $(call rt_test,test_amend,$(T)/test_amend.c,asan-ub))
$(eval $(call rt_test,test_signal,$(T)/test_signal.c,asan-ub))
$(eval $(call rt_test,test_conform,$(T)/test_conform.c,asan-ub))
$(eval $(call rt_test,test_workflow_park,$(T)/test_workflow_park.c,asan-ub))
$(eval $(call rt_test,test_park_inflight,$(T)/test_park_inflight.c,asan-ub))
$(eval $(call rt_test,test_course_slots,$(T)/test_course_slots.c,asan-ub,-rdynamic))
$(eval $(call rt_test,test_foreign,$(T)/test_foreign.c,asan-ub,-rdynamic))
$(eval $(call rt_test,test_desc_sizes,$(T)/test_desc_sizes.c,asan-ub,-rdynamic))
$(eval $(call rt_test,test_saga,$(T)/test_saga.c,asan-ub,-rdynamic))
$(eval $(call rt_test,test_bytes,$(T)/test_bytes.c,asan-ub,-rdynamic))
$(eval $(call rt_test,test_attrs,$(T)/test_attrs.c,asan-ub,-rdynamic))
$(eval $(call rt_test,test_hardening_values,$(T)/test_hardening_values.c,asan-ub))
$(eval $(call rt_test,test_hardening_tables,$(T)/test_hardening_tables.c,asan-ub))
$(eval $(call rt_test,test_hardening_park,$(T)/test_hardening_park.c,asan-ub))
$(eval $(call rt_test,test_hardening_threads,$(T)/test_hardening_threads.c,asan-ub,$(WRAP_TIMEDWAIT)))
$(eval $(call rt_test,test_events,$(T)/test_events.c,asan-ub-events))
$(eval $(call rt_test,test_course_saga,$(T)/test_course_saga.c,asan-ub-ckpt,-rdynamic))
$(eval $(call rt_test,test_amend_saga,$(T)/test_amend_saga.c,asan-ub-ckpt,-rdynamic))
$(eval $(call rt_test,test_boundary,$(T)/test_boundary.c,asan-ub-registrar,-rdynamic))

# Under ThreadSanitizer
$(eval $(call rt_test,test_thread_tsan,$(T)/test_thread.c,tsan))
$(eval $(call rt_test,test_future_tsan,$(T)/test_future.c,tsan))
$(eval $(call rt_test,host_tsan,skeleton/host.c,tsan,-rdynamic -I $(OUT),$(HELLO_HEADER)))
$(eval $(call rt_test,test_process_tsan,$(T)/test_process.c,tsan,-rdynamic))
$(eval $(call rt_test,test_store_stress_tsan,$(T)/test_store_stress.c,tsan,-rdynamic))
$(eval $(call rt_test,test_hardening_threads_tsan,$(T)/test_hardening_threads.c,tsan,$(WRAP_TIMEDWAIT)))
$(eval $(call rt_test,test_hardening_park_tsan,$(T)/test_hardening_park.c,tsan))
$(eval $(call rt_test,test_terms_tsan,$(T)/test_terms.c,tsan))
$(eval $(call rt_test,test_remedy_tsan,$(T)/test_remedy.c,tsan))
$(eval $(call rt_test,test_cancel_tsan,$(T)/test_cancel.c,tsan))
$(eval $(call rt_test,test_handoff_tsan,$(T)/test_handoff.c,tsan))
$(eval $(call rt_test,test_bind_user_tsan,$(T)/test_bind_user.c,tsan))
$(eval $(call rt_test,test_signal_tsan,$(T)/test_signal.c,tsan))
$(eval $(call rt_test,test_workflow_park_tsan,$(T)/test_workflow_park.c,tsan))
$(eval $(call rt_test,test_park_inflight_tsan,$(T)/test_park_inflight.c,tsan))
$(eval $(call rt_test,test_course_tsan,$(T)/test_course.c,tsan))
$(eval $(call rt_test,test_course_park_tsan,$(T)/test_course_park.c,tsan))
$(eval $(call rt_test,test_amend_tsan,$(T)/test_amend.c,tsan))
$(eval $(call rt_test,test_conform_tsan,$(T)/test_conform.c,tsan))
$(eval $(call rt_test,test_events_tsan,$(T)/test_events.c,tsan-events))
$(eval $(call rt_test,test_course_saga_tsan,$(T)/test_course_saga.c,tsan-ckpt,-rdynamic))
$(eval $(call rt_test,test_amend_saga_tsan,$(T)/test_amend_saga.c,tsan-ckpt,-rdynamic))

# ---- the compiler ----

# The compiler, C++23 with no exceptions and no RTTI, configured and built by
# CMake. The plain binary lands in target/bin over the objects in
# target/build, and SANITIZE=ON builds it under ASan and UBSan into
# target/bin-asan over target/build-asan instead, so the plain binary is
# never silently swapped for the instrumented one. CMake decides what needs
# recompiling; the touch keeps make from asking again when nothing did.
SANITIZE ?= OFF
GEAS_PLAIN := target/bin/geas
GEAS_ASAN  := target/bin-asan/geas
COMPILER_SRC := compiler/CMakeLists.txt $(wildcard compiler/src/*.cpp compiler/src/*.hpp)

ifeq ($(SANITIZE),ON)
compiler: $(GEAS_ASAN)
else
compiler: $(GEAS_PLAIN)
endif

$(GEAS_PLAIN): $(COMPILER_SRC)
	cmake -S compiler -B target/build -DGEAS_SANITIZE=OFF \
	    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(abspath target/bin)
	cmake --build target/build --parallel
	@touch $@

$(GEAS_ASAN): $(COMPILER_SRC)
	cmake -S compiler -B target/build-asan -DGEAS_SANITIZE=ON \
	    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(abspath target/bin-asan)
	cmake --build target/build-asan --parallel
	@touch $@

# ---- the compiled modules ----

# Every module one gate or another loads is built by exactly one rule, so two
# gates running side by side never write the same module C or the same
# library while a third loads it. A module rebuilds when the compiler, the
# runtime, the ABI header, or any std module changes, since imports reach
# into lib/std.
STD_SOURCES := $(wildcard lib/std/*.geas)
MODULE_DEPS = $(GEAS) $(RT_SO) runtime/include/geas/geas_abi.h $(STD_SOURCES)

$(OUT)/lib%.geas.so: skeleton/%.geas $(MODULE_DEPS)
	$(GEAS) build $<

$(OUT)/lib%.geas.so: $(T)/%.geas $(MODULE_DEPS)
	$(GEAS) build $<

$(OUT)/lib%.geas.so: interop/grpc/contracts/%.geas $(MODULE_DEPS)
	$(GEAS) build $<

$(OUT)/lib%.geas.so: $(OUT)/%.geas $(MODULE_DEPS)
	$(GEAS) build $<

$(MODULE_SVC): examples/supervisor/service.geas $(MODULE_DEPS)
	$(GEAS) build examples/supervisor/service.geas

$(BIN_DEMO): skeleton/main_demo.geas $(MODULE_DEPS)
	$(GEAS) build --bin skeleton/main_demo.geas

# The host includes the header emit-header writes for hello.geas, so the
# mangled name it resolves follows the module's signature hash
$(HELLO_HEADER): $(GEAS) skeleton/hello.geas
	@mkdir -p $(OUT)
	$(GEAS) emit-header skeleton/hello.geas

$(REQ_HEADER): $(GEAS) $(T)/requirements.geas
	@mkdir -p $(OUT)
	$(GEAS) emit-header $(T)/requirements.geas

module: $(MODULE)

host: $(HOST)

$(HOST): skeleton/host.c $(RT_SO) $(HELLO_HEADER)
	$(CC) $(CFLAGS) -pthread -I runtime/include -I $(OUT) skeleton/host.c -L $(OUT) -lgeasrt \
	    -Wl,-rpath,'$$ORIGIN' -o $(HOST)

# ---- make check, every test once ----

FRONT_GATES := test-golden test-codegen
LANG_GATES := smoke test-soname test-iname test-partial test-lang test-std test-bin \
    test-header test-proto test-determinism test-requirements test-python
RUNTIME_GATES := test-runtime test-wire test-thread test-future test-process test-dusk
STORE_GATES := test-store-unit test-store test-store-txn test-store-fail test-store-crash \
    test-store-python test-park test-lifecycle
WORKFLOW_GATES := test-conform test-boundary test-course test-course-park test-course-slots \
    test-foreign test-desc-sizes test-amend test-events test-events-python test-bytes \
    test-bytes-python test-attrs test-terms test-remedy test-cancel test-handoff \
    test-bind-user test-signal test-workflow-park test-park-inflight test-saga \
    test-saga-python test-handoff-python test-course-saga test-course-python \
    test-amend-saga test-amend-python test-hardening
TSAN_GATES := tsan tsan-workflow tsan-hardening tsan-process
CHECK_GATES := $(FRONT_GATES) $(LANG_GATES) $(RUNTIME_GATES) $(STORE_GATES) \
    $(WORKFLOW_GATES) $(TSAN_GATES)

all: check

check: $(CHECK_GATES)
	@echo "[check] ok"

# The quick tier: the front end and codegen goldens, the walking skeleton,
# the language lowering gate, and the host driven saga
quick: test-golden test-codegen smoke test-lang test-saga
	@echo "[quick] ok"

# The front end goldens and the codegen goldens, see the top of each runner.
# The front end suite runs its standalone programs against the runtime
# library, so it waits for it.
test-golden: $(GEAS) $(RT_SO)
	GEAS=$(GEAS_ABS) tests/run.sh

test-codegen: $(GEAS)
	GEAS=$(GEAS_ABS) compiler/tests/run_codegen.sh

smoke: $(RT_SO) $(MODULE) $(HOST)
	./$(HOST)

# The module names its runtime and loads through a host linked against it
test-soname: $(RT_SO) $(MODULE) $(T)/test_soname.c
	readelf -d $(RT_VERSIONED) | grep -E '\(SONAME\).*\[libgeasrt\.so\.0\]'
	readelf -d $(MODULE) | grep -E '\(NEEDED\).*\[libgeasrt\.so\.0\]'
	$(CC) $(CFLAGS) -pthread -I runtime/include $(T)/test_soname.c \
	    -L $(OUT) -lgeasrt -Wl,-rpath,'$$ORIGIN' -o $(OUT)/test_soname
	./$(OUT)/test_soname

# The leak gate. The runtime compiles straight into the host here so LSan sees
# every allocation, and -rdynamic exports the geas_* symbols the dlopened
# module resolves against. valgrind covers the same ground where installed.
smoke-asan: $(OUT)/host_asan $(MODULE)
	./$(OUT)/host_asan

# The runtime's own unit gate: the deep value helpers, deep copy, and the
# copy-in isolation the memory model promises. Compiled with the runtime in
# one binary under ASan and LSan so every instance allocation is watched.
test-runtime: $(OUT)/test_value
	./$(OUT)/test_value

# The wire codec gate under ASan: the canonical value encoding against its
# checked-in byte goldens in tests/wire, the encode(decode(b)) == b canonicity
# claim, and a negative corpus of truncations, forbidden tags, lying lengths,
# and a depth bomb, every refusal watched for leaks. Regenerate the goldens
# with ./$(OUT)/test_wire --emit tests/wire after a deliberate format change.
test-wire: $(OUT)/test_wire
	./$(OUT)/test_wire tests/wire

# The store driver gate under ASan and LSan: the SQLite backend against a real
# temp file database, the golden row dump in tests/store, the write-then-read
# round trip for every scalar type, the commit and rollback of a transaction,
# and a negative corpus of a dead dsn, broken SQL, a wrong parameter count, and
# a non scalar parameter, every refusal watched for leaks. The sqlite object is
# uninstrumented for speed, which ASan tolerates, while store.c and the test are
# under the sanitizer. Regenerate the golden with ./$(OUT)/test_store --emit
# tests/store after a deliberate format change.
test-store-unit: $(OUT)/test_store
	./$(OUT)/test_store tests/store

# The S1 store gate under ASan and LSan: the compiled Ledger signed against a
# temp SQLite file, the schema reconciled into a fresh file, a row written and
# read back, an update round tripped, a missing account answered as the
# contract's own Err, an injection string bound as a value that leaves the table
# standing, and the two refusal signs, a divergent schema (GEAS_ERR_TYPE) and a
# missing dsn vow (GEAS_ERR_UNBOUND). The runtime, the driver, and the
# uninstrumented sqlite object are linked in so LSan watches every row that
# lands on the instance to zero leaks; -rdynamic exports the geas_* symbols the
# dlopened module resolves against.
test-store: $(OUT)/test_store_ledger $(MODULE_LEDGER)
	./$(OUT)/test_store_ledger

# The S2 transactional gate under ASan and LSan: the Ledger's Transfer
# subcontract driven as one all-or-nothing episode. A good transfer commits both
# writes and the file reflects the moved balances; a failed transfer rolls the
# debit back so nothing durable survives; a second call to a resolved
# transactional pledge is GEAS_ERR_STATE; and a break mid transaction leaves no
# debit durable. Every persistence assertion reopens the file in a fresh
# instance so the file, not a cache, is the witness.
test-store-txn: $(OUT)/test_store_txn $(MODULE_LEDGER)
	./$(OUT)/test_store_txn

# The S3 failure gate under ASan and LSan: GEAS_ERR_STORE proven to be the store
# failing the runtime and nothing else, and the business boundary proven to hold
# against it. A read only connection made from a mode=ro dsn refuses every write
# with GEAS_ERR_STORE, a loose insert and a transactional debit both, the latter
# rolled back; a duplicate primary key is the backend's own constraint refusal,
# GEAS_ERR_STORE and distinct from a value Err, and the table stands after it; a
# contended writer that loses the file to an open transaction surfaces
# GEAS_ERR_STORE rather than a stall. The guard the milestone turns on is here
# too: an overdraft is Err(Insufficient), the ledger's own rule as a value with
# an GEAS_OK delivery, never once a store status. Every persistence claim reopens
# the file in a fresh instance so the file is the witness.
test-store-fail: $(OUT)/test_store_fail $(MODULE_LEDGER)
	./$(OUT)/test_store_fail

# The S3 durability gate under ASan and LSan: the rollback on crash proof the
# whole store layer rests on. It forks a child that opens a transaction, buffers
# a debit, signals over a pipe, and is SIGKILLed dead in the transaction; the
# parent reopens the file and reads the account back to find the buffered debit
# gone, rolled back on the next open. A sign kill loop drives that crash over and
# over against one file, the balance never drifting and the reconcile never
# refusing the shape, so a run of crashes cannot corrupt the store. The child
# opens its own runtime because a pool does not cross a fork; it is SIGKILLed so
# no sanitizer watches its allocations, while the parent shuts down clean.
test-store-crash: $(OUT)/test_store_crash $(MODULE_LEDGER)
	./$(OUT)/test_store_crash

# The S3 concurrency gate under ASan and LSan: many worker threads sign one
# Ledger instance per transfer, each its own connection to one shared file, and
# storm the pool with transactional transfers. The loser of a write race is
# GEAS_ERR_STORE and an overdraft is a business Err, but every transfer is whole
# either way, so the money in the pool is conserved to the last unit no matter
# how the races fall. The gate seeds a known total, runs the storm, and reads it
# back on one thread: a drift is a half committed episode, and there is none.
# Not in check, since it is minutes of load on ground test-store-txn covers;
# run it on its own.
test-store-stress: $(OUT)/test_store_stress $(MODULE_LEDGER)
	./$(OUT)/test_store_stress

# The S3 concurrency gate under ThreadSanitizer: the same storm of many
# instances on one file, so TSan watches the pool, the waiters, and the per
# instance lock that keeps each connection single threaded. The dlopened module
# and the uninstrumented sqlite object are tolerated the way every other TSan
# gate tolerates them; -rdynamic exports the geas_* symbols the module resolves
# against. Not in check either; run it explicitly.
test-store-stress-tsan: $(OUT)/test_store_stress_tsan $(MODULE_LEDGER)
	./$(OUT)/test_store_stress_tsan

# The S4 Python store gate: the ctypes binding drives the compiled Ledger
# against a temp SQLite file with no C written and no generated code, the store
# twin of test-python. It signs with a dsn override, opens
# accounts and reads their balances, rewrites a row, commits a good transfer and
# rolls a bad one back, and asserts the same outcomes the C hosts assert, proving
# the store is invisible to a foreign host. Skips cleanly where python3 is not
# installed. ASan is not needed here, the runtime is a subprocess and the C store
# gates already watch its allocations; libgeasrt is the ordinary build.
test-store-python: $(RT_SO) $(MODULE_LEDGER)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 interop/python/demo_ledger.py; \
	else \
	    echo "[test-store-python] python3 not found, skipping"; \
	fi

# The instance surface gate under ASan and LSan: the compiled lifecycle
# module walks sign, status(), the vow read through an instance, park,
# break, and resume entirely inside the language and answers the stations
# as one string; the host signs Driver, points park_dsn at a temp file, and
# asserts the answer plus the two fault paths, a dead dsn as GEAS_ERR_STORE
# and an unparked key as GEAS_ERR_NAME, riding the thunk's exit convention.
test-lifecycle: $(OUT)/test_lifecycle $(MODULE_LIFE)
	./$(OUT)/test_lifecycle

# The parked instance gate under ASan and LSan: an instance's durable state
# written into a store row and stood back up in a fresh runtime, the vows,
# the latches, the Err payloads, and the transactional fates all crossing.
# A partial payment resumes with its override and runs to fulfilled, an
# automatic break resumes with its payload readable, and a store backed
# Ledger resumes against its own dsn vow, reads the committed balance, and
# refuses to rerun its closed episode. The refusals hold: park mid
# transaction, park with a walk in the air, park after the caller's own
# break, resume of an unparked key, a lying hash, and a bare runtime.
test-park: $(OUT)/test_park $(MODULE_PAY) $(MODULE_LEDGER)
	./$(OUT)/test_park

# The workflow gates, each under ASan, LSan, and UBSan with the runtime
# linked in, each over handwritten descriptors on a fake clock, so a
# deadline is a number the test moves and a retry delay is an entry in a list
# the test reads.

# The terms gate: after refused early and admitted once its items landed,
# within counted from eligibility and lapsing the item, a subcontract's
# deadline lapsing what inside it has not latched, the retry count, delay
# sequence, cap, and "on" filter, the contract term, the descriptors a sign
# refuses, and the V1 registrar reading a descriptor of the old size.
test-terms: $(OUT)/test_terms
	./$(OUT)/test_terms

# The remedy gate: newest latch first with the pledge's Ok value, Remedying
# seen from the body and from a second thread, a failing remedy recorded
# without stopping the rest, the three roads into a break, the fulfilled
# contract nothing is undone for, the unbound remedy, and a host break racing
# the pass.
test-remedy: $(OUT)/test_remedy
	./$(OUT)/test_remedy

# Cancellation waits for bodies, undoes their Ok outcomes, and wakes retries
test-cancel: $(OUT)/test_cancel
	./$(OUT)/test_cancel

# The hand off: a cancel request a body reads while the instance stays
# live, a release with no remedy and no state change, its last autopark row,
# its rolled back episode, a course it cuts carried on by the claimer, and a
# claim that takes the row in one move, with exactly one winner of a race
test-handoff: $(OUT)/test_handoff
	./$(OUT)/test_handoff

# Shared host bodies keep the user pointer fixed at sign and resume
test-bind-user: $(OUT)/test_bind_user
	./$(OUT)/test_bind_user

# The course gate: handwritten state machines in the emitted shape driven
# by the runtime on a fake clock, straight steps, a failing step and its
# remedies, await, sleep, select and loops across re-entries, cancel while
# running and while waiting, wake races, events, the ticker, and a descriptor
# from before the course still loading.
test-course: $(OUT)/test_course
	./$(OUT)/test_course

# A course across a park: autopark rows at every step boundary, children
# killed at each boundary and inside bodies then resumed here, a park while
# waiting, a sleep's deadline, a changed course, rows from before the frame,
# a checkpoint the store refuses, a park inside a call's retry wait and from
# a remedy body, a together's outcomes across a park, and host driven rows.
test-course-park: $(OUT)/test_course_park
	./$(OUT)/test_course_park

# The course slot types: a fresh frame at the smallest value of each slot's
# type, a row whose list slot carries a String refused at resume and claim,
# the registrar's slot table checks, and rows mutated inside the course
# frame, each refused or resumed and run, under ASan and UBSan. check mutates
# CHECK_SLOT_ROUNDS rows and soak the full SOAK_SLOT_ROUNDS.
CHECK_SLOT_ROUNDS ?= 1000
SOAK_SLOT_ROUNDS ?= 10000

test-course-slots: $(OUT)/test_course_slots $(OUT)/libcourse_slots.geas.so
	GEAS_FUZZ_ROUNDS=$(CHECK_SLOT_ROUNDS) ./$(OUT)/test_course_slots

# The foreign symbols: abstract pledges and remedies resolved through the
# function their symbol names in this executable, linked with -rdynamic, a
# missing symbol refusing the sign, a host binding beating the symbol, and
# the same resolution at resume and claim, under ASan and UBSan.
test-foreign: $(OUT)/test_foreign $(OUT)/libforeign.geas.so
	./$(OUT)/test_foreign

# The descriptor sizes: a descriptor ending at a guard page registered at
# every historical size and every eight byte step to the whole struct, so a
# read past the reported size faults, and only a V7 one sees its symbols.
test-desc-sizes: $(OUT)/test_desc_sizes
	./$(OUT)/test_desc_sizes

# Amendment: a row parked under one version resumed as a later one through
# an amends block, handwritten descriptor pairs on a fake clock. Renames,
# retirements, added items and their seeds, vows carried, defaulted and
# computed, signal payloads, lapses, eligibility, remedy records,
# transactional fates, retry progress, the course frame kept, restarted,
# dropped and refused, a together's progress, every flow blob version, the
# counts, the events, and the registrar.
test-amend: $(OUT)/test_amend
	./$(OUT)/test_amend

# Events preserve instance order and run with runtime locks released
test-events: $(OUT)/test_events
	./$(OUT)/test_events

test-events-python: $(RT_SO) $(MODULE_PAY)
	python3 interop/python/test_events.py

# The signal gate: delivery latches, the payload is the instance's own copy
# and the first one wins, a signal is an atom and an after target, its own
# after and within hold, and the pledge and signal surfaces refuse each
# other's names.
test-signal: $(OUT)/test_signal
	./$(OUT)/test_signal

# The checked boundary gate: every rule of the type table at a fulfillment
# argument, the returns, remedies, signals, vow bindings, by reference
# arguments, amends, and stored rows held to it, the registrar refusing a
# table that does not hold up, a descriptor with no table behaving as it
# always did, and park rows and wire encodings mutated byte by byte under a
# fixed seed, every answer a status. check mutates CHECK_CONFORM_ROUNDS
# encodings and soak the full SOAK_CONFORM_ROUNDS, with each TSan round at
# TSAN_CONFORM_ROUNDS.
CHECK_CONFORM_ROUNDS ?= 300
SOAK_CONFORM_ROUNDS ?= 3000
TSAN_CONFORM_ROUNDS ?= 200

test-conform: $(OUT)/test_conform
	./$(OUT)/test_conform $(CHECK_CONFORM_ROUNDS)

# The workflow park gate: eligibility times, lapses, Ok values, signal
# payloads, and remedy progress across a park into a fresh runtime, a pass
# parked in the middle finishing after the resume, a row in the format that
# predates the workflow state still resuming, and an unknown blob version
# refused.
test-workflow-park: $(OUT)/test_workflow_park
	./$(OUT)/test_workflow_park

# Retry progress and remedy snapshots across a host shutdown
test-park-inflight: $(OUT)/test_park_inflight
	./$(OUT)/test_park_inflight

# The saga gate, the workflow surface end to end: the compiler builds the
# order saga and the flowdesk contracts, and the C host loads both, binds
# the saga's abstract steps, and drives it on a fake clock through the happy
# path, an early step, a retried authorize, a failed ship and its remedies,
# a lapsed signal, the contract's term running out, and a park and resume,
# then has a Geas body hand a signal to a contract it signed and checks the
# Time, Duration, and generic constructor values.
MODULE_SAGA := $(OUT)/libsaga.geas.so
MODULE_DESK := $(OUT)/libflowdesk.geas.so

test-saga: $(OUT)/test_saga $(MODULE_SAGA) $(MODULE_DESK)
	./$(OUT)/test_saga

# The saga from Python: the geas package loads the same saga module, binds
# Python callables over the abstract steps, points the runtime at a Python
# clock, and walks the happy path and a break path with its remedies.
test-saga-python: $(RT_SO) $(MODULE_SAGA)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 interop/python/demo_saga.py; \
	else \
	    echo "[test-saga-python] python3 not found, skipping"; \
	fi

# The hand off from Python: a cancel request a bound body reads, a park and
# a release with no refund, and a claim in a second runtime that lands the
# saga Fulfilled while a second claim finds nothing
test-handoff-python: $(RT_SO) $(MODULE_SAGA)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 -B interop/python/test_handoff.py; \
	else \
	    echo "[test-handoff-python] python3 not found, skipping"; \
	fi

# The engine driven saga end to end: the compiler builds
# skeleton/order_course.geas and a twin whose course text differs and
# nothing else, and the C host drives the compiled course on a fake clock,
# the happy path, a failing step and its remedies, a retried step, the
# select's timeout, a lapsed signal, the together's two calls at once, a
# kill and resume at every step boundary and inside every host body, the
# changed course refused, a host fulfillment refused, host breaks while the
# course waits and runs, the term running out, and the events. The runtime is
# built with its checkpoint renamed so the test can stand in front of it, see
# the top of the test.
MODULE_COURSE_SAGA := $(OUT)/liborder_course.geas.so
COURSE_SAGA_CHANGED := $(OUT)/order_course_changed.geas

$(COURSE_SAGA_CHANGED): skeleton/order_course.geas
	@mkdir -p $(OUT)
	sed 's/timeout 2d ->/timeout 1d ->/' skeleton/order_course.geas > $@.tmp
	mv $@.tmp $@

course-saga-modules: $(MODULE_COURSE_SAGA) $(OUT)/liborder_course_changed.geas.so

test-course-saga: $(OUT)/test_course_saga course-saga-modules
	./$(OUT)/test_course_saga

# The checked boundary end to end: the compiler builds skeleton/boundary.geas
# and skeleton/boundary_host.geas, and a C host drives them with values built
# wrong on purpose, at every argument, answer, signal, vow, remedy, course
# call, and park row, each refused with GEAS_ERR_TYPE before a body runs and
# each beside a conforming value that runs one. The runtime is built with its
# registrar renamed so the test can count the compiled bodies, see the top of
# the test.
boundary-modules: $(OUT)/libboundary.geas.so $(OUT)/libboundary_host.geas.so

test-boundary: $(OUT)/test_boundary boundary-modules
	./$(OUT)/test_boundary

# Amendment end to end: the compiler builds skeleton/order_course.geas at
# version 1 and skeleton/order_course_v2.geas at version 2, whose amends 1
# block reads the recorded shape in skeleton/shapes, plus a version 2 with
# the block cut out, a version 1 whose shape drifted from the recorded one,
# and the host driven saga at version 2 from tests/runtime. Recording the
# sources again is a no op while their shapes are the ones on file and a
# refusal once they are not, which keeps the recorded shapes honest; the
# record runs once, in the one rule below, before the version 2 build reads
# what it holds. The C host parks version 1 sagas and resumes them as
# version 2: mid course with the course restarted, a step failing after the
# amendment, a second park as version 2, a kill at every step boundary, the
# module without the block and the drifted row refused, and the host driven
# saga.
AMEND_BARE := $(OUT)/order_course_bare.geas
AMEND_DRIFT := $(OUT)/order_course_drift.geas
AMEND_SAGA_DIR := $(OUT)/amend_saga_v1
AMEND_RECORDED := $(OUT)/amend-recorded.stamp

$(AMEND_RECORDED): $(GEAS) skeleton/order_course.geas skeleton/order_course_v2.geas \
        skeleton/saga.geas $(T)/shapes/OrderSaga.v1.shape $(wildcard skeleton/shapes/*.shape)
	@mkdir -p $(AMEND_SAGA_DIR)/shapes
	$(GEAS) record skeleton/order_course.geas
	$(GEAS) record skeleton/order_course_v2.geas
	cp skeleton/saga.geas $(AMEND_SAGA_DIR)/saga.geas
	cp $(T)/shapes/OrderSaga.v1.shape $(AMEND_SAGA_DIR)/shapes/
	$(GEAS) record $(AMEND_SAGA_DIR)/saga.geas
	@touch $@

$(OUT)/liborder_course_v2.geas.so: $(AMEND_RECORDED)
$(OUT)/libsaga_v2.geas.so: $(AMEND_RECORDED)

$(AMEND_BARE): skeleton/order_course_v2.geas
	@mkdir -p $(OUT)
	sed '/^    amends 1 {/,/^    }/d' skeleton/order_course_v2.geas > $@.tmp
	! grep -q '^    amends' $@.tmp
	mv $@.tmp $@

$(AMEND_DRIFT): skeleton/order_course.geas
	@mkdir -p $(OUT)
	sed 's/^    vow grace: Duration = 30m$$/&\n    vow note: String = "drift"/' skeleton/order_course.geas > $@.tmp
	grep -q 'vow note' $@.tmp
	mv $@.tmp $@

amend-saga-modules: $(AMEND_RECORDED) $(MODULE_COURSE_SAGA) $(OUT)/liborder_course_v2.geas.so \
    $(OUT)/liborder_course_bare.geas.so $(OUT)/liborder_course_drift.geas.so \
    $(MODULE_SAGA) $(OUT)/libsaga_v2.geas.so

test-amend-saga: $(OUT)/test_amend_saga amend-saga-modules
	./$(OUT)/test_amend_saga

# The engine driven saga from Python: the geas package loads the same order_course
# module, binds Python callables over the abstract steps, points the
# runtime at a Python clock with the runtime's own ticker running, and walks
# the happy path, a break path with its remedies, and a park into a fresh
# runtime that resumes the course where it waited.
test-course-python: $(RT_SO) $(MODULE_COURSE_SAGA)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 interop/python/demo_course.py; \
	else \
	    echo "[test-course-python] python3 not found, skipping"; \
	fi

# Amendment from Python: the geas package parks a version 1 order saga at its select
# in one Runtime and resumes it in a fresh one that loaded version 2, which
# amends the row, restarts the course, and finishes the saga.
test-amend-python: amend-saga-modules
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 interop/python/demo_amend.py; \
	else \
	    echo "[test-amend-python] python3 not found, skipping"; \
	fi

# The hardening gate under ASan, LSan, and UBSan: the tables with no fixed
# size and geas_contract_free, values at the edges of the codec, the walk,
# and the helpers, park rows and their generations, saved deadlines, a clock
# below zero, and the threads at the edges, each case in a child of its own,
# each test binary a gate of its own so they run side by side. The tables
# test copies the empty module to many paths and runs its reuse case
# CHECK_REUSE_ROUNDS rounds here and SOAK_REUSE_ROUNDS under soak; the
# threads test wraps pthread_cond_timedwait to read every deadline the
# runtime waits on. The Python scope check runs where python3 is.
HARD_EMPTY := $(OUT)/libhardening_empty.so
CHECK_REUSE_ROUNDS ?= 10000
SOAK_REUSE_ROUNDS ?= 100000
HARDENING_GATES := test-hardening-values test-hardening-tables test-hardening-park \
    test-hardening-threads test-hardening-scope

$(HARD_EMPTY): $(T)/hardening_module.c runtime/include/geas/geas.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -I runtime/include $(T)/hardening_module.c -o $@

test-hardening: $(HARDENING_GATES)

test-hardening-values: $(OUT)/test_hardening_values
	./$(OUT)/test_hardening_values

test-hardening-tables: $(OUT)/test_hardening_tables $(HARD_EMPTY)
	HARD_EMPTY_MODULE=$(HARD_EMPTY) GEAS_REUSE_ROUNDS=$(CHECK_REUSE_ROUNDS) \
	    ./$(OUT)/test_hardening_tables

test-hardening-park: $(OUT)/test_hardening_park
	./$(OUT)/test_hardening_park

test-hardening-threads: $(OUT)/test_hardening_threads
	./$(OUT)/test_hardening_threads

test-hardening-scope: $(RT_SO) $(MODULE_PAY) $(T)/test_python_scope.py
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 -B $(T)/test_python_scope.py; \
	else \
	    echo "[test-hardening] python3 not found, skipping the scope check"; \
	fi

# The threading gate under ASan: the pool, the per-instance serialization,
# out-of-order waits, and the break race, with every allocation watched.
test-thread: $(OUT)/test_thread
	./$(OUT)/test_thread

# Future receipts are polled, delivered, and released without accumulating;
# queued work retains its own hold when a break forfeits a released receipt
test-future: $(OUT)/test_future
	./$(OUT)/test_future

# The iname gate under ASan: two compiled generations loaded side by side,
# exact name discovery, the version mismatch miss, the freeze, and the
# canonical dump against its golden, tests/runtime/iname.expect.
# The split pair beside it: two modules whose contract and pledge names
# join to one text, each held to an entry and a body of its own
MODULE_SPLIT := $(OUT)/libiname_split_one.geas.so $(OUT)/libiname_split_two.geas.so

test-iname: $(OUT)/test_iname $(MODULE) $(MODULE_V2) $(MODULE_SPLIT)
	./$(OUT)/test_iname

# The requirements gate under ASan: the compiled payment module's policy
# trees drive the per-pledge latch, the evaluator's priority order, the
# partial result surface, and the automatic break that keeps its heap for
# the errors it reports.
test-partial: $(OUT)/test_partial $(MODULE_PAY)
	./$(OUT)/test_partial

# The language lowering gate under ASan: the compiled gauntlet module walks
# every construct the code generator lowers, loops, assignment in all three
# lvalue forms, match with payload bindings, propagation, deep equality, and
# the out of bounds index rule.
test-lang: $(OUT)/test_lang $(MODULE_LANG)
	./$(OUT)/test_lang

# The standard library gate under ASan: the std_user module merges four
# std modules through the loader, and the host signs MathOps, ListOps,
# and StdUser out of the one library, driving the integer and float
# arithmetic, the list reductions and their Option answers, the error sums
# in the Err box, and the sort that runs through an incorporated clause.
test-std: $(OUT)/test_std $(MODULE_STD)
	./$(OUT)/test_std

# Program steps and their cancellation run through a compiled std.process consumer
test-process: $(OUT)/test_process $(OUT)/libsteps.geas.so
	./$(OUT)/test_process

tsan-process: $(OUT)/test_process_tsan $(OUT)/libsteps.geas.so
	./$(OUT)/test_process_tsan

# Both dusk demos over the modules built here, see the top of the script
test-dusk: $(GEAS) $(RT_SO) $(MODULE_SAGA) $(MODULE_COURSE_SAGA)
	GEAS_PREBUILT=1 interop/dusk/run_demo.sh

# The Python interop gate: the ctypes package in interop/python/geas/ drives
# the payment walk and the by reference protocol with no C written and no
# generated code, which is the product claim that the documented ABI alone
# is enough for a foreign host. Skips cleanly where python3 is not installed.
# There is no TSan variant on purpose: the TSan runtime cannot be mixed into an
# uninstrumented python3 process, so the concurrency surface stays covered
# by the C tsan gate.
test-python: $(RT_SO) $(MODULE) $(MODULE_PAY)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 interop/python/demo_payment.py && \
	    python3 interop/python/test_future.py && \
	    python3 interop/python/test_cancel.py; \
	else \
	    echo "[test-python] python3 not found, skipping"; \
	fi

# The standalone gate: build --bin turns main_demo into an executable, and
# both exits the grammar promises are asserted from the shell. Three args on
# the Ok path exit 3, the word fail takes the Err path, which must exit 1
# and carry the rendered payload on stderr while stdout stays silent.
test-bin: $(BIN_DEMO)
	./$(BIN_DEMO) a b c; test $$? -eq 3
	./$(BIN_DEMO); test $$? -eq 0
	./$(BIN_DEMO) x fail y >$(OUT)/main_demo.out 2>$(OUT)/main_demo.err; \
	    code=$$?; test $$code -eq 1 \
	    && grep -q 'asked to fail' $(OUT)/main_demo.err \
	    && test ! -s $(OUT)/main_demo.out
	@echo "[test-bin] ok"

# The header gate: emit-header must reproduce the pinned golden byte for
# byte, and the C smoke host compiles against nothing but that header and
# geas.h, resolves the mangled name it spells, and signs under its hash.
test-header: $(HELLO_HEADER) $(OUT)/test_header $(MODULE)
	diff tests/golden/hello.geas.h $(HELLO_HEADER)
	./$(OUT)/test_header

# Byte buffers through compiled pledges, references, wire values, and parking
test-bytes: $(OUT)/test_bytes $(OUT)/libblob.geas.so
	./$(OUT)/test_bytes

test-bytes-python: $(RT_SO) $(OUT)/libblob.geas.so
	python3 interop/python/test_bytes.py

# Declared settings before and after sign, with a second build of the same shape
ATTRS_DIR := $(OUT)/attrs

$(ATTRS_DIR)/jobattrs.geas: skeleton/jobattrs.geas
	@mkdir -p $(ATTRS_DIR)
	cp skeleton/jobattrs.geas $@

$(ATTRS_DIR)/jobstep_changed.geas: skeleton/jobstep.geas
	@mkdir -p $(ATTRS_DIR)
	sed 's/priority: -2/priority: -3/' skeleton/jobstep.geas > $@.tmp
	mv $@.tmp $@

$(OUT)/libjobstep_changed.geas.so: $(ATTRS_DIR)/jobstep_changed.geas $(ATTRS_DIR)/jobattrs.geas $(MODULE_DEPS)
	$(GEAS) build $(ATTRS_DIR)/jobstep_changed.geas

test-attrs: $(OUT)/test_attrs $(OUT)/libjobstep.geas.so $(OUT)/libjobstep_changed.geas.so
	./$(OUT)/test_attrs
	python3 interop/python/test_attrs.py

# Written policies, a full byte index table, and the structural default
# through 255 items, compiled and driven by a C host under ASan
test-requirements: $(OUT)/test_requirements $(OUT)/librequirements.geas.so
	./$(OUT)/test_requirements

# The proto gate: emit-proto must reproduce every pinned golden byte for
# byte, the .proto a stock protoc consumes and the Go session wrapper that
# carries the stream whose lifetime is the instance's. The shape hash the
# wrapper pins is the same value the module registers, so a drift between
# the bridge surface and the compiled contract shows up here as a diff. The
# goldens walk the bridged type set: payment is the scalar walk, hello an
# enum sum in the Err arm, ledger Unit and a bare sum, main_demo a payload
# sum and a repeated parameter under two services, and std_user the
# multi-contract prefix rule, Option and List wrappers, and the reserved
# SignPledgeRequest disambiguation, with its wrapper pinning four session
# types out of one file. The gauntlet closes the set: its Maps cross as
# ordered entry list wrappers, the insertion order the language pins and
# proto's own map type would lose, and its tuple as a numbered item
# message, so the file that was the standing refusal is now a golden. The
# gate emits into a directory of its own, so the bridge gates' surfaces in
# target/geas-out are theirs alone.
PROTO_DIR := $(OUT)/proto-gate
PROTO_GATE_OUT := $(PROTO_DIR)/$(OUT)

# Runs the compiler from a scratch directory of the gate's own, with lib
# beside it so std imports resolve the way they do at the root
in_scratch = mkdir -p $(1) && ln -sfn $(CURDIR)/lib $(1)/lib && \
	cd $(1) && GEAS_ROOT=$(CURDIR) $(GEAS_ABS)

test-proto: $(GEAS)
	rm -rf $(PROTO_DIR)
	for s in payment hello ledger main_demo std_user lang; do \
	    ( $(call in_scratch,$(PROTO_DIR)) emit-proto $(CURDIR)/skeleton/$$s.geas ) || exit 1; \
	done
	diff tests/golden/payment.proto $(PROTO_GATE_OUT)/payment.proto
	diff tests/golden/payment_session.go $(PROTO_GATE_OUT)/payment_session.go
	diff tests/golden/hello.proto $(PROTO_GATE_OUT)/hello.proto
	diff tests/golden/ledger.proto $(PROTO_GATE_OUT)/ledger.proto
	diff tests/golden/main_demo.proto $(PROTO_GATE_OUT)/main_demo.proto
	diff tests/golden/payment_session.ts $(PROTO_GATE_OUT)/payment_session.ts
	diff tests/golden/std_user.proto $(PROTO_GATE_OUT)/std_user.proto
	diff tests/golden/std_user_session.go $(PROTO_GATE_OUT)/std_user_session.go
	diff tests/golden/std_user_session.ts $(PROTO_GATE_OUT)/std_user_session.ts
	diff tests/golden/lang.proto $(PROTO_GATE_OUT)/lang.proto
	@echo "[test-proto] ok"

# The determinism gate: two builds of the same source must emit byte
# identical module C, which is what keeps every mangled name and shape hash
# in the iname story reproducible. Both builds run in a directory of the
# gate's own, so the modules the other gates load are never rebuilt under
# them.
DET_DIR := $(OUT)/determinism
DET_OUT := $(DET_DIR)/$(OUT)
DET_SOURCES := hello hello_v2 payment lang std_user

test-determinism: $(GEAS) $(RT_SO)
	rm -rf $(DET_DIR)
	for s in $(DET_SOURCES); do \
	    ( $(call in_scratch,$(DET_DIR)) build $(CURDIR)/skeleton/$$s.geas ) || exit 1; \
	    mv $(DET_OUT)/$$s.c $(DET_OUT)/$$s.c.first || exit 1; \
	    ( $(call in_scratch,$(DET_DIR)) build $(CURDIR)/skeleton/$$s.geas ) || exit 1; \
	    diff $(DET_OUT)/$$s.c.first $(DET_OUT)/$$s.c || exit 1; \
	done
	( $(call in_scratch,$(DET_DIR)) build --bin $(CURDIR)/skeleton/main_demo.geas )
	mv $(DET_OUT)/main_demo.c $(DET_OUT)/main_demo.c.first
	mv $(DET_OUT)/main_demo.main.c $(DET_OUT)/main_demo.main.c.first
	( $(call in_scratch,$(DET_DIR)) build --bin $(CURDIR)/skeleton/main_demo.geas )
	diff $(DET_OUT)/main_demo.c.first $(DET_OUT)/main_demo.c
	diff $(DET_OUT)/main_demo.main.c.first $(DET_OUT)/main_demo.main.c
	( $(call in_scratch,$(DET_DIR)) emit-header $(CURDIR)/skeleton/hello.geas )
	mv $(DET_OUT)/hello.geas.h $(DET_OUT)/hello.geas.h.first
	( $(call in_scratch,$(DET_DIR)) emit-header $(CURDIR)/skeleton/hello.geas )
	diff $(DET_OUT)/hello.geas.h.first $(DET_OUT)/hello.geas.h
	rm -rf $(DET_DIR)
	@echo "[determinism] ok"

# ---- the TSan gates, one round each ----

# The same concurrency surface under ThreadSanitizer: the stress gate and the
# full host walk, runtime linked into each binary so TSan sees every lock.
# The dlopened module is not instrumented, which TSan tolerates; the runtime
# and host side of every race is.
tsan: tsan-thread tsan-future tsan-host

tsan-thread: $(OUT)/test_thread_tsan
	./$(OUT)/test_thread_tsan

tsan-future: $(OUT)/test_future_tsan
	./$(OUT)/test_future_tsan

tsan-host: $(OUT)/host_tsan $(MODULE)
	./$(OUT)/host_tsan

# The hardening gate's park rows and thread cases under ThreadSanitizer
tsan-hardening: tsan-hardening-threads tsan-hardening-park

tsan-hardening-threads: $(OUT)/test_hardening_threads_tsan
	./$(OUT)/test_hardening_threads_tsan

tsan-hardening-park: $(OUT)/test_hardening_park_tsan
	./$(OUT)/test_hardening_park_tsan

# The workflow gates under ThreadSanitizer: retries with the instance lock
# released, remedy passes and signals across threads, user bindings, and
# the courses, handwritten and compiled, one gate per test and one round
# each; soak repeats the ones whose races want many rounds
TSAN_WORKFLOW := terms remedy cancel handoff bind_user signal workflow_park park_inflight \
    events course course_park amend course_saga amend_saga conform
TSAN_WORKFLOW_GATES := $(addprefix tsan-,$(subst _,-,$(TSAN_WORKFLOW)))

tsan-workflow: $(TSAN_WORKFLOW_GATES)

define tsan_gate
tsan-$(subst _,-,$(1)): $(OUT)/test_$(1)_tsan $(2)
	./$(OUT)/test_$(1)_tsan $(3)
endef
$(foreach name,terms remedy cancel handoff bind_user signal workflow_park park_inflight \
    events course course_park amend,$(eval $(call tsan_gate,$(name))))
$(eval $(call tsan_gate,course_saga,course-saga-modules))
$(eval $(call tsan_gate,amend_saga,amend-saga-modules))
$(eval $(call tsan_gate,conform,,$(TSAN_CONFORM_ROUNDS)))

# ---- make soak, the long tail ----

# Every repeated round is a make target of its own, soak-round-<name>-<n>,
# so the rounds of every test run side by side across the machine, each into
# a log of its own that is printed only when the round fails. The rounds per
# test are the counts the races want; SOAK_ROUNDS sets every one of them at
# once, and SOAK_JOBS how many run at a time when make was not given -j.
SOAK_JOBS ?= $(shell nproc 2>/dev/null || echo 4)
SOAK_LOGS := $(OUT)/soak

# One repeated soak test: $(1) its name, $(2) the binary, $(3) its rounds,
# $(4) its arguments, $(5) what it needs built first
define soak_test
SOAK_ROUNDS_$(1) := $$(or $$(SOAK_ROUNDS),$(3))
soak-$(1): $$(addprefix soak-round-$(1)-,$$(shell seq 1 $$(SOAK_ROUNDS_$(1))))
	@echo "[soak] $(1) ok over $$(SOAK_ROUNDS_$(1)) rounds"
soak-round-$(1)-%: $(OUT)/$(2) $(5)
	@mkdir -p $$(SOAK_LOGS)
	@./$(OUT)/$(2) $(4) > $$(SOAK_LOGS)/$(1).$$*.log 2>&1 || \
	    { echo "[soak] $(1) round $$* failed"; cat $$(SOAK_LOGS)/$(1).$$*.log; exit 1; }
	@rm -f $$(SOAK_LOGS)/$(1).$$*.log
SOAK_TESTS += soak-$(1)
.PHONY: soak-$(1)
endef

$(eval $(call soak_test,tsan-hardening-threads,test_hardening_threads_tsan,5))
$(eval $(call soak_test,tsan-cancel,test_cancel_tsan,20))
$(eval $(call soak_test,tsan-handoff,test_handoff_tsan,30))
$(eval $(call soak_test,tsan-workflow-park,test_workflow_park_tsan,20))
$(eval $(call soak_test,tsan-park-inflight,test_park_inflight_tsan,20))
$(eval $(call soak_test,tsan-events,test_events_tsan,20))
$(eval $(call soak_test,tsan-course,test_course_tsan,30))
$(eval $(call soak_test,tsan-course-park,test_course_park_tsan,30))
$(eval $(call soak_test,tsan-amend,test_amend_tsan,20))
$(eval $(call soak_test,tsan-course-saga,test_course_saga_tsan,30,,course-saga-modules))
$(eval $(call soak_test,tsan-amend-saga,test_amend_saga_tsan,30,,amend-saga-modules))
$(eval $(call soak_test,tsan-conform,test_conform_tsan,10,$(TSAN_CONFORM_ROUNDS)))

# The full fuzz counts and the long reuse case, one run each
soak-conform: $(OUT)/test_conform
	./$(OUT)/test_conform $(SOAK_CONFORM_ROUNDS)

soak-course-slots: $(OUT)/test_course_slots $(OUT)/libcourse_slots.geas.so
	GEAS_FUZZ_ROUNDS=$(SOAK_SLOT_ROUNDS) ./$(OUT)/test_course_slots

soak-reuse: $(OUT)/test_hardening_tables $(HARD_EMPTY)
	HARD_EMPTY_MODULE=$(HARD_EMPTY) GEAS_REUSE_ROUNDS=$(SOAK_REUSE_ROUNDS) \
	    ./$(OUT)/test_hardening_tables reuse_rounds

SOAK_TESTS += soak-conform soak-course-slots soak-reuse

# make soak runs at the machine's width on its own; under a make already
# given -j it joins that make's jobs instead
soak:
	@$(MAKE) --no-print-directory $(if $(findstring jobserver,$(MAKEFLAGS)),,-j$(SOAK_JOBS)) soak-all

soak-all: $(SOAK_TESTS)
	@echo "[soak] ok"

# ---- make check-net, the bridge and the supervisor ----

# The gRPC bridge: compiled contracts served over the surface geas emits, by
# one generic server. bridge_server.py loads each module through the C ABI,
# binds the abstract bodies a --handlers file supplies, and reads each
# emitted .proto at startup into descriptors, so the server side carries no
# generated stub and no line written for one contract. The clients are the
# other half, built from the same emitted files with stock tooling: Python
# stubs from grpc_tools, Go stubs from protoc beside the emitted Go session
# wrapper, and Node over @grpc/proto-loader with the emitted TypeScript
# wrapper. The stubs are generated, never tracked, so a stale stub cannot
# outlive a change to a contract's surface. Out of check: every gate needs
# grpcio, the Go and Node ones their toolchains, and each skips clean where
# what it needs is absent. make check-net runs them all with the supervisor
# gates, and each server listens on a port no other gate uses, every
# surface and stub is built by one rule, and every park store is a temp
# file, so they run side by side.
#
# grpcio is not in the system python3 here, so grpc-venv builds a venv the
# gates prefer. A gate takes the venv if it has grpc, else the system
# python3 if that has grpc, else skips clean the way test-python does.
GRPCVENV := target/grpcvenv
GRPC_GEN := target/grpc-gen
GRPC_HANDLERS := interop/grpc/handlers
BRIDGE_SERVER := interop/grpc/bridge_server.py
BRIDGE_CLIENT := interop/grpc/bridge_client.py
BRIDGE_PAY := --module $(MODULE_PAY) --handlers $(GRPC_HANDLERS)/payment.py
BRIDGE_SAGA := --module $(MODULE_SAGA) --handlers $(GRPC_HANDLERS)/saga.py
BRIDGE_COURSE := --module $(MODULE_COURSE_SAGA) --handlers $(GRPC_HANDLERS)/order_course.py

NET_GATES := test-grpc-bridge test-grpc-go test-grpc-node test-grpc-resume test-grpc-failover \
    test-grpc-saga test-grpc-course test-grpc-saga-go test-grpc-saga-node test-grpc-edge \
    test-grpc-handoff test-supervisor test-supervisor-watch

check-net: $(NET_GATES)
	@echo "[check-net] ok"

grpc-venv:
	@if [ ! -x $(GRPCVENV)/bin/python ]; then \
	    echo "[grpc-venv] creating $(GRPCVENV)"; \
	    python3 -m venv $(GRPCVENV) && \
	    $(GRPCVENV)/bin/pip install --quiet --disable-pip-version-check \
	        grpcio grpcio-tools; \
	fi
	@$(GRPCVENV)/bin/python -c \
	    "import grpc; print('[grpc-venv] grpcio ' + grpc.__version__)"

# The python a gRPC gate runs under, left in py, empty when none has grpc
GRPC_FIND_PY = py=""; \
	if $(GRPCVENV)/bin/python -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="$(GRPCVENV)/bin/python"; \
	elif command -v python3 >/dev/null 2>&1 && \
	     python3 -c "import grpc, grpc_tools" 2>/dev/null; then \
	    py="python3"; \
	fi

# One source's emitted surface, the .proto and its two session wrappers,
# written by the one rule for that source
$(OUT)/%.proto $(OUT)/%_session.go $(OUT)/%_session.ts: skeleton/%.geas $(GEAS)
	$(GEAS) emit-proto $<

$(OUT)/%.proto $(OUT)/%_session.go $(OUT)/%_session.ts: interop/grpc/contracts/%.geas $(GEAS)
	$(GEAS) emit-proto $<

# The Python stubs over one emitted surface, left unwritten where no python
# has grpc, in which case every gate that reads them skips
$(GRPC_GEN)/%_pb2.py: $(OUT)/%.proto
	@$(GRPC_FIND_PY); \
	if [ -n "$$py" ]; then \
	    mkdir -p $(GRPC_GEN) && \
	    $$py -m grpc_tools.protoc -I $(OUT) --python_out=$(GRPC_GEN) \
	        --grpc_python_out=$(GRPC_GEN) $<; \
	fi

grpc_stubs = $(foreach s,$(1),$(GRPC_GEN)/$(s)_pb2.py)

# The payment contract over the generic server, driven from Python: the
# demo_payment.py walk with the same answers across a process boundary, a
# pledge's Err as a value on an OK rpc and a Geas status as a gRPC code, an
# instance that lives exactly as long as its Session stream, a killed
# client's instance broken at once, and a quiet client keeping its instance
# however long it says nothing. The same server also serves the language
# gauntlet, whose lists, maps, tuples, and options cross through nothing but
# reflection over lang.proto.
test-grpc-bridge: $(RT_SO) $(MODULE_PAY) $(MODULE_LANG) $(call grpc_stubs,payment lang)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-bridge] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	$$py $(BRIDGE_SERVER) --port 50251 $(BRIDGE_PAY) --module $(MODULE_LANG) & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	$$py $(BRIDGE_CLIENT) --port 50251 --legacy-ttl 2.0 && \
	$$py $(BRIDGE_CLIENT) --port 50251 --lang-walk; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# Whether go, protoc, and protoc's two Go plugins are all at hand
GRPC_HAS_GO = command -v go >/dev/null 2>&1 && command -v protoc >/dev/null 2>&1 && \
	PATH="$$PATH:$$HOME/go/bin" command -v protoc-gen-go >/dev/null 2>&1 && \
	PATH="$$PATH:$$HOME/go/bin" command -v protoc-gen-go-grpc >/dev/null 2>&1

# Every surface the Go client imports: each emitted .proto through protoc and
# its Go plugins, the emitted session wrapper copied into the same package,
# then the build, once for both Go gates. The packages are generated, never
# tracked, and nothing is built where the toolchain is absent, in which case
# both Go gates skip.
GO_DIR := interop/grpc/go
GO_SURFACES := payment saga order_course
GO_CLIENT := $(OUT)/bridge_client_go

$(GO_CLIENT): $(foreach s,$(GO_SURFACES),$(OUT)/$(s).proto) $(wildcard $(GO_DIR)/client/*.go $(GO_DIR)/go.mod)
	@if { $(GRPC_HAS_GO); }; then \
	    for s in $(GO_SURFACES); do \
	        mkdir -p $(GO_DIR)/$${s}pb; \
	        PATH="$$PATH:$$HOME/go/bin" protoc -I $(OUT) \
	            --go_out=$(GO_DIR) --go_opt=module=geasbridge \
	            --go-grpc_out=$(GO_DIR) --go-grpc_opt=module=geasbridge \
	            $(OUT)/$$s.proto || exit 1; \
	        cp $(OUT)/$${s}_session.go $(GO_DIR)/$${s}pb/ || exit 1; \
	    done; \
	    (cd $(GO_DIR) && go build -o ../../../$(GO_CLIENT) ./client) || exit 1; \
	fi

# The same server, driven by a Go client built from nothing but geas's
# emitted artifacts: the whole payment lifecycle, session stream, pinned
# shape hash, value Err, and the dead client's instance collected in
# milliseconds. Two languages meet at one emitted surface with no hand
# written stub between them.
test-grpc-go: $(RT_SO) $(MODULE_PAY) $(GO_CLIENT)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-go] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	if ! { $(GRPC_HAS_GO); }; then \
	    echo "[test-grpc-go] go or the protoc toolchain not found, skipping"; \
	    exit 0; \
	fi; \
	$$py $(BRIDGE_SERVER) --port 50252 $(BRIDGE_PAY) & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	./$(GO_CLIENT) -addr 127.0.0.1:50252; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# The session that survives. A park enabled server signs a session whose
# stream then drops, the instance parks under its token, the server itself
# is killed and a fresh one stands up on the same park store, and the client
# resumes by token: the latch set before the death answers the walk that
# finishes after it. Any replica holding the park store can resume the
# session, which is the whole answer to replica affinity.
test-grpc-resume: $(RT_SO) $(MODULE_PAY) $(call grpc_stubs,payment)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-resume] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	parkdb=$$(mktemp target/ashparkgrpc_XXXXXX); \
	$$py $(BRIDGE_SERVER) --port 50256 $(BRIDGE_PAY) --park-dsn "$$parkdb" & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	token=$$($$py $(BRIDGE_CLIENT) --port 50256 --park-walk | tail -n 1); \
	if [ -z "$$token" ]; then \
	    echo "[test-grpc-resume] the park walk failed"; \
	    kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	    rm -f "$$parkdb"; exit 1; \
	fi; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	$$py $(BRIDGE_SERVER) --port 50256 $(BRIDGE_PAY) --park-dsn "$$parkdb" & \
	srv=$$!; \
	$$py $(BRIDGE_CLIENT) --port 50256 --resume-walk "$$token"; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	rm -f "$$parkdb"; \
	if [ $$code -eq 0 ]; then echo "[test-grpc-resume] ok"; fi; \
	exit $$code

# Two replicas, one park store, and the claim that makes them honest.
# Replica A parks a session and dies by SIGKILL, replica B resumes it on
# nothing but the shared store. Then A comes back, the walk parks eight
# sessions on B, and both replicas race one Resume for each token at the
# same moment. The runtime's claim resumes and deletes the row in one move,
# so every round has exactly one winner and a loser answering NOT_FOUND, the
# same answer a spent token earns.
test-grpc-failover: $(RT_SO) $(MODULE_PAY) $(call grpc_stubs,payment)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-failover] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	parkdb=$$(mktemp target/ashparkgrpc_XXXXXX); \
	$$py $(BRIDGE_SERVER) --port 50257 $(BRIDGE_PAY) --park-dsn "$$parkdb" & \
	srva=$$!; \
	$$py $(BRIDGE_SERVER) --port 50258 $(BRIDGE_PAY) --park-dsn "$$parkdb" & \
	srvb=$$!; \
	trap 'kill -9 $$srva $$srvb 2>/dev/null; wait $$srva $$srvb 2>/dev/null; rm -f "$$parkdb" "$$parkdb"-journal' EXIT INT TERM; \
	token=$$($$py $(BRIDGE_CLIENT) --port 50257 --park-walk | tail -n 1); \
	if [ -z "$$token" ]; then \
	    echo "[test-grpc-failover] the park walk on replica A failed"; \
	    exit 1; \
	fi; \
	kill -9 $$srva 2>/dev/null; wait $$srva 2>/dev/null; \
	$$py $(BRIDGE_CLIENT) --port 50258 --resume-walk "$$token" || exit 1; \
	$$py $(BRIDGE_SERVER) --port 50257 $(BRIDGE_PAY) --park-dsn "$$parkdb" & \
	srva=$$!; \
	$$py $(BRIDGE_CLIENT) --port 50257 --peer-port 50258 --race-resume --rounds 8; \
	code=$$?; \
	if [ $$code -eq 0 ]; then echo "[test-grpc-failover] ok"; fi; \
	exit $$code

# Whether node and npm are at hand, and the emitted surfaces the Node walks
# load: each .proto rides in through @grpc/proto-loader at runtime, no
# protoc and no generated stub, and each session through its emitted
# TypeScript wrapper, which node runs directly under type stripping. The
# copy and the npm install run once for both Node gates, and not at all
# where node is absent.
GRPC_HAS_NODE = command -v node >/dev/null 2>&1 && command -v npm >/dev/null 2>&1
NODE_DIR := interop/grpc/node
NODE_SURFACES := payment saga order_course
NODE_GEN := $(OUT)/node-gen.stamp

$(NODE_GEN): $(foreach s,$(NODE_SURFACES),$(OUT)/$(s).proto) $(NODE_DIR)/package.json
	@if { $(GRPC_HAS_NODE); }; then \
	    mkdir -p $(NODE_DIR)/gen; \
	    for s in $(NODE_SURFACES); do \
	        cp $(OUT)/$$s.proto $(OUT)/$${s}_session.ts $(NODE_DIR)/gen/ || exit 1; \
	    done; \
	    if [ ! -d $(NODE_DIR)/node_modules ]; then \
	        (cd $(NODE_DIR) && npm install --no-audit --no-fund) || exit 1; \
	    fi; \
	    touch $@; \
	fi

# The Node twin of the Go gate: the same server, driven by a client whose
# whole diet is two npm packages and the emitted artifacts. This is the
# editor extension path, proven end to end: no native code crosses the
# marketplace boundary.
test-grpc-node: $(RT_SO) $(MODULE_PAY) $(NODE_GEN)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-node] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	if ! { $(GRPC_HAS_NODE); }; then \
	    echo "[test-grpc-node] node or npm not found, skipping"; \
	    exit 0; \
	fi; \
	$$py $(BRIDGE_SERVER) --port 50254 $(BRIDGE_PAY) & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	node $(NODE_DIR)/client.mjs --addr 127.0.0.1:50254; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# Signals over the bridge. The host driven order saga, skeleton/saga.geas,
# served by the same generic server with its gateway and carrier bodies from
# a handlers file, driven from Python: capture refused before authorize, the
# delivered signal refused before ship and landing after it through its own
# Signal rpc, the saga Fulfilled, and a refused shipment whose Err arm
# arrives as a value while the break line fires and the remedies run.
test-grpc-saga: $(RT_SO) $(MODULE_SAGA) $(call grpc_stubs,saga)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-saga] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	$$py $(BRIDGE_SERVER) --port 50261 $(BRIDGE_SAGA) & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	$$py $(BRIDGE_CLIENT) --port 50261 --saga-walk; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# The engine driven order saga over the bridge, from Python. The surface
# carries no pledge rpc: Session signs, the course runs on the server's
# runtime through the handlers file's bodies, the server's ticker brings its
# waits due, and the client watches it through GetCourse, hands the
# delivered signal in, and reads the course finished and the saga
# Fulfilled. The server runs with a park store, so a saga resting at its
# select parks when its stream closes and resumes at the same step.
test-grpc-course: $(RT_SO) $(MODULE_COURSE_SAGA) $(call grpc_stubs,order_course)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-course] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	parkdb=$$(mktemp target/ashparkgrpc_XXXXXX); \
	$$py $(BRIDGE_SERVER) --port 50262 $(BRIDGE_COURSE) --park-dsn "$$parkdb" & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; rm -f "$$parkdb"' EXIT INT TERM; \
	$$py $(BRIDGE_CLIENT) --port 50262 --course-walk --park; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# The saga and the course from Go, through the emitted Go session wrappers
# and their signal and course helpers, against one server holding both.
test-grpc-saga-go: $(RT_SO) $(MODULE_SAGA) $(MODULE_COURSE_SAGA) $(GO_CLIENT)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-saga-go] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	if ! { $(GRPC_HAS_GO); }; then \
	    echo "[test-grpc-saga-go] go or the protoc toolchain not found, skipping"; \
	    exit 0; \
	fi; \
	$$py $(BRIDGE_SERVER) --port 50263 $(BRIDGE_SAGA) $(BRIDGE_COURSE) & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	./$(GO_CLIENT) -addr 127.0.0.1:50263 -walk saga && \
	./$(GO_CLIENT) -addr 127.0.0.1:50263 -walk course; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# The saga and the course from Node, through the emitted TypeScript
# wrappers and their signal and course helpers, against one server holding
# both.
test-grpc-saga-node: $(RT_SO) $(MODULE_SAGA) $(MODULE_COURSE_SAGA) $(NODE_GEN)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-saga-node] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	if ! { $(GRPC_HAS_NODE); }; then \
	    echo "[test-grpc-saga-node] node or npm not found, skipping"; \
	    exit 0; \
	fi; \
	$$py $(BRIDGE_SERVER) --port 50264 $(BRIDGE_SAGA) $(BRIDGE_COURSE) & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null' EXIT INT TERM; \
	node $(NODE_DIR)/client.mjs --addr 127.0.0.1:50264 --walk saga && \
	node $(NODE_DIR)/client.mjs --addr 127.0.0.1:50264 --walk course; \
	code=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; \
	exit $$code

# The bridge's endings and limits, each against a small contract under
# interop/grpc/contracts with protoc stubs over its emitted .proto. Stall's
# hold body returns only once a cancel request reaches it, so a Break and a
# dropped stream beside a hold in flight must both ask before they wait,
# and both return with the server still answering. A server of six workers
# with two reserved admits four sessions, answers a unary call with all
# four live, refuses a fifth with RESOURCE_EXHAUSTED, and a pool that
# leaves no session room refuses to start. NestedOption crosses None,
# Some(None), and Some(Some(x)) both ways. skeleton/boundary.geas takes the
# requests a proto client can send that are not the declared type, a string
# that is not UTF-8, a message nested past the parser, a Char key outside
# Unicode's scalar values, a map key sent twice, each INVALID_ARGUMENT, and
# its supplied bodies answer outside their declared returns on cue, each
# INTERNAL with nothing latched.
BRIDGE_EDGE := interop/grpc/edge_client.py
BRIDGE_STALL = --module $(OUT)/libstall.geas.so --handlers $(GRPC_HANDLERS)/stall.py
EDGE_MODULES := $(OUT)/libstall.geas.so $(OUT)/libnested_option.geas.so $(OUT)/libboundary.geas.so

test-grpc-edge: $(RT_SO) $(EDGE_MODULES) $(call grpc_stubs,stall nested_option boundary)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-edge] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	if $$py $(BRIDGE_SERVER) --port 50265 --max-workers 2 --reserve-workers 2 \
	        $(BRIDGE_STALL) >/dev/null; then \
	    echo "[test-grpc-edge] a pool with no session room started"; \
	    exit 1; \
	fi; \
	$$py $(BRIDGE_SERVER) --port 50265 $(BRIDGE_STALL) \
	    --module $(OUT)/libnested_option.geas.so & \
	srv=$$!; \
	$$py $(BRIDGE_SERVER) --port 50266 --max-workers 6 --reserve-workers 2 $(BRIDGE_STALL) & \
	srvb=$$!; \
	$$py $(BRIDGE_SERVER) --port 50267 --module $(OUT)/libboundary.geas.so \
	    --handlers $(GRPC_HANDLERS)/boundary.py & \
	srvc=$$!; \
	trap 'kill $$srv $$srvb $$srvc 2>/dev/null; wait $$srv $$srvb $$srvc 2>/dev/null' EXIT INT TERM; \
	$$py $(BRIDGE_EDGE) --port 50265 --stall-walk && \
	$$py $(BRIDGE_EDGE) --port 50265 --nested-walk && \
	$$py $(BRIDGE_EDGE) --port 50267 --boundary-walk && \
	$$py $(BRIDGE_EDGE) --port 50266 --admission-walk 4; \
	code=$$?; \
	kill $$srv $$srvb $$srvc 2>/dev/null; wait $$srv $$srvb $$srvc 2>/dev/null; \
	if [ $$code -eq 0 ]; then echo "[test-grpc-edge] ok"; fi; \
	exit $$code

# The hand off: Handoff's course waits for a signal and its Geas written
# remedy writes a row per run to the store its dsn vow names. A session
# broken outright runs it once; a session whose stream drops is parked and
# released and runs it never; resumed and broken, it runs once more.
test-grpc-handoff: $(RT_SO) $(OUT)/libhandoff.geas.so $(call grpc_stubs,handoff)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-grpc-handoff] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	parkdb=$$(mktemp target/ashparkgrpc_XXXXXX); \
	stores=$$(mktemp -d target/ashundogrpc_XXXXXX); \
	$$py $(BRIDGE_SERVER) --port 50268 --module $(OUT)/libhandoff.geas.so \
	    --park-dsn "$$parkdb" & \
	srv=$$!; \
	trap 'kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; rm -f "$$parkdb" "$$stores"/undo_*.db; rmdir "$$stores"' EXIT INT TERM; \
	$$py $(BRIDGE_EDGE) --port 50268 --handoff-walk "$$stores"; \
	code=$$?; \
	if [ $$code -eq 0 ]; then echo "[test-grpc-handoff] ok"; fi; \
	exit $$code

# The flagship example gate: the supervisor whose service state machine is a
# contract instance. The driver walks the whole story: two services come up
# Partial, the flaky one crashes into Broken twice with its crash count read
# off the shared Runs table and is given up, the steady one is parked when
# the supervisor is terminated, survives as a process, is resumed by a fresh
# supervisor under the same pid, and is stopped clean into Fulfilled.
test-supervisor: $(RT_SO) $(MODULE_SVC)
	@if command -v python3 >/dev/null 2>&1; then \
	    python3 examples/supervisor/test_supervisor.py; \
	else \
	    echo "[test-supervisor] python3 not found, skipping"; \
	fi

# The observer half of the flagship: a remote client watching the services
# over the supervisor's own read-only surface. Deliberately not the emitted
# contract surface: an observer must not be able to sign, fulfill, or break
# anything, so the supervisor serves its own two rpcs and the contract's
# diagnosis rides them, the state name, the partial name lists, and the
# crash count off the store. Skips clean without grpcio like the bridge
# gates.
test-supervisor-watch: $(RT_SO) $(MODULE_SVC)
	@$(GRPC_FIND_PY); \
	if [ -z "$$py" ]; then \
	    echo "[test-supervisor-watch] grpcio not found, skipping (make grpc-venv)"; \
	    exit 0; \
	fi; \
	mkdir -p $(GRPC_GEN); \
	$$py -m grpc_tools.protoc -I examples/supervisor \
	    --python_out=$(GRPC_GEN) --grpc_python_out=$(GRPC_GEN) \
	    examples/supervisor/observer.proto || exit 1; \
	$$py examples/supervisor/test_watch.py

# ---- install ----

# The installed toolchain. install lays the artifacts out the way a system
# expects them: the geas binary on PATH, libgeasrt where the linker already
# searches, the runtime headers under include/geas so the emitted C's
# <geas/geas.h> resolves with no flags, and the std modules under
# share/geas/std where the loader's system probe finds them. PREFIX defaults
# to /usr/local for a hand install; a package build passes PREFIX=/usr and
# DESTDIR at the fakeroot.
PREFIX ?= /usr/local

install: $(GEAS) $(RT_SO)
	install -Dm755 $(GEAS) $(DESTDIR)$(PREFIX)/bin/geas
	install -Dm755 $(RT_VERSIONED) $(DESTDIR)$(PREFIX)/lib/$(RT_SONAME)
	ln -sfn $(RT_SONAME) $(DESTDIR)$(PREFIX)/lib/libgeasrt.so
	install -Dm644 -t $(DESTDIR)$(PREFIX)/include/geas \
	    runtime/include/geas/geas.h runtime/include/geas/geas_abi.h \
	    runtime/include/geas/geas_wire.h runtime/include/geas/geas_store.h
	install -Dm644 -t $(DESTDIR)$(PREFIX)/share/geas/std lib/std/*.geas

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/geas
	rm -f $(DESTDIR)$(PREFIX)/lib/libgeasrt.so $(DESTDIR)$(PREFIX)/lib/$(RT_SONAME)
	rm -rf $(DESTDIR)$(PREFIX)/include/geas
	rm -rf $(DESTDIR)$(PREFIX)/share/geas

clean:
	rm -rf target

.PHONY: all check quick soak soak-all check-net runtime compiler module host smoke smoke-asan \
    course-saga-modules boundary-modules amend-saga-modules grpc-venv install uninstall clean \
    $(CHECK_GATES) $(HARDENING_GATES) tsan-thread tsan-future tsan-host \
    tsan-hardening-threads tsan-hardening-park $(TSAN_WORKFLOW_GATES) \
    test-store-stress test-store-stress-tsan soak-conform soak-course-slots soak-reuse \
    $(NET_GATES)

# The headers every runtime object and gate binary read, recorded as they
# were compiled
-include $(wildcard $(RT_DIR)/*/*.d $(OUT)/*.d)
