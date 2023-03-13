# Entry points

.PHONY: entry
entry:
	@echo "make all: build all benchmarks"
	@echo "make run: run all benchmarks (important tests only)"
	@echo "make run-perm_count: run the 'perm_count' benchmark"
	@echo "make run-par_perm_count: run the parallel 'perm_count' benchmark (requires OCaml 5)"
	@echo "make run-synthetic: run the 'synthetic' benchmark"
	@echo "make run-globroots: run the 'globroots' benchmark"
	@echo "make run-local_roots: run the 'local_roots' benchmark"
	@echo "(replace run with hyper to use hyperfine)"
	@echo "make test: test boxroots on 'perm_count' and test ocaml-boxroot-sys"
	@echo "make clean"
	@echo
	@echo "Note: for each benchmark-running target you can set TEST_MORE={1,2}"
	@echo "to enable some less-important benchmarks that are disabled by default"
	@echo "  make run-globroots TEST_MORE=1"
	@echo "other options: BOXROOT_DEBUG=1, STATS=1"

.PHONY: all
all:
	dune build @all

.PHONY: clean
clean:
	dune clean

EMPTY=
REF_IMPLS=\
  boxroot \
  gc \
  $(EMPTY)
REF_IMPLS_MORE=\
  ocaml \
  generational \
  $(EMPTY)
REF_IMPLS_MORE_MORE=\
  ocaml_ref \
  dll_boxroot \
  bitmap_boxroot \
  rem_boxroot \
  global \
  $(EMPTY)

ifeq ($(TEST_MORE),2)
TEST_MORE_MORE=1
endif

DUNE_EXEC = dune exec --display=quiet
HYPER = hyperfine --warmup 1 --min-runs 20

check_tsc = \
  sh -c "if [ tsc != `cat /sys/devices/system/clocksource/clocksource0/current_clocksource` ]; then echo \"Warning: /sys/devices/system/clocksource/clocksource0/current_clocksource is not tsc\";fi;";

run_bench = \
	$(check_tsc) \
  echo "Benchmark: $(1)" \
  && echo "---" \
  $(foreach REF, $(REF_IMPLS) $(if $(TEST_MORE),$(REF_IMPLS_MORE),) \
	               $(if $(TEST_MORE_MORE),$(REF_IMPLS_MORE_MORE),), \
    && ($(2) "REF=$(REF) $(3)") \
  ) \
	&& echo "---"

run_perm_count = \
	$(call run_bench,"perm_count", $(1), \
	  CHOICE=persistent N=10 $(DUNE_EXEC) ./benchmarks/perm_count.exe)

run_par_perm_count = \
	$(call run_bench,"par_perm_count", $(1), \
	  CHOICE=persistent N=10 DOMS=4 $(DUNE_EXEC) ./benchmarks/par_perm_count.exe)

run_synthetic = \
	$(call run_bench,"synthetic", $(1), \
	    N=8 \
	    SMALL_ROOTS=10_000 \
	    YOUNG_RATIO=1 \
	    LARGE_ROOTS=20 \
	    SMALL_ROOT_PROMOTION_RATE=0.2 \
	    LARGE_ROOT_PROMOTION_RATE=1 \
	    ROOT_SURVIVAL_RATE=0.99 \
	    GC_PROMOTION_RATE=0.1 \
	    GC_SURVIVAL_RATE=0.5 \
	    $(DUNE_EXEC) ./benchmarks/synthetic.exe \
	)

run_globroots = \
	$(call run_bench,"globroots", $(1), \
	  N=500_000 $(DUNE_EXEC) ./benchmarks/globroots.exe)

run_local_roots = \
	$(check_tsc) \
	echo "Benchmark: local_roots" \
	&& echo "---" \
	$(foreach N, 1 2 $(if $(TEST_MORE),3 4,) 5 $(if $(TEST_MORE),8,) 10 \
		           $(if $(TEST_MORE),30,) 100 $(if $(TEST_MORE),300,) 1000, \
	  $(foreach ROOT, boxroot local $(if $(TEST_MORE), ocaml generational naive) \
                    $(if $(TEST_MORE_MORE), ocaml_ref dll_boxroot bitmap_boxroot rem_boxroot global), \
	    && ($(1) "N=$(N) ROOT=$(ROOT) $(DUNE_EXEC) ./benchmarks/local_roots.exe") \
	  ) && echo "---")

.PHONY: run-perm_count hyper-perm_count
run-perm_count: all
	$(call run_perm_count, sh -c)
hyper-perm_count: all
	$(call run_perm_count, $(HYPER))

.PHONY: run-par_perm_count hyper-par_perm_count
run-par_perm_count: all
	$(call run_par_perm_count, sh -c)
hyper-par_perm_count: all
	$(call run_par_perm_count, $(HYPER))

.PHONY: run-synthetic hyper-synthetic
run-synthetic: all
	$(call run_synthetic, sh -c)
hyper-synthetic: all
	$(call run_synthetic, $(HYPER))

.PHONY: run-globroots hyper-globroots
run-globroots: all
	$(call run_globroots, sh -c)
hyper-globroots: all
	$(call run_globroots, $(HYPER))

.PHONY: run-local_roots hyper-local_roots
run-local_roots: all
	$(call run_local_roots, sh -c)
hyper-local_roots: all
	$(call run_local_roots, $(HYPER))

.PHONY: run hyper
run:
	$(MAKE) run-perm_count
	$(MAKE) run-synthetic
	$(MAKE) run-globroots
	$(MAKE) run-local_roots
hyper:
	$(MAKE) hyper-perm_count
	$(MAKE) hyper-synthetic
	$(MAKE) hyper-globroots
	$(MAKE) hyper-local_roots

.PHONY: run-more hyper-more
run-more:
	$(MAKE) run TEST_MORE=1
hyper-more:
	$(MAKE) hyper TEST_MORE=1

.PHONY: test-boxroot
test-boxroot: all
	N=10 REF=boxroot CHOICE=ephemeral $(DUNE_EXEC) benchmarks/perm_count.exe

.PHONY: test-rs
test-rs:
	cd rust/ocaml-boxroot-sys && \
	RUSTFLAGS="-D warnings" cargo build --features "link-ocaml-runtime-and-dummy-program" --verbose && \
	RUSTFLAGS="-D warnings" cargo test --features "link-ocaml-runtime-and-dummy-program" --verbose

.PHONY: clean-rs
clean-rs:
	cd rust/ocaml-boxroot-sys && \
	cargo clean

.PHONY: test
test: test-boxroot test-rs
