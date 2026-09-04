# Makefile — a thin wrapper around CMakePresets.json (debug, release,
# tsan), the same shape as sequencer's. Read `make help`.

# Wherever the owner put it; exported in ~/.bashrc on the development
# machine, but not in every shell. check-vcpkg-root fails loudly.
VCPKG_ROOT ?=
export VCPKG_ROOT
export PATH := $(if $(VCPKG_ROOT),$(VCPKG_ROOT):)$(PATH)

PRESET ?= debug

TEST_FILTER ?=
CTEST_FLAGS := --output-on-failure
ifneq ($(strip $(TEST_FILTER)),)
CTEST_FLAGS += -R "$(TEST_FILTER)"
endif

SBE_JAR    := vendor/sbe/sbe-all-1.40.1.jar
SCHEMA     := schema/exchange.xml
GENERATED  := generated
LIQUIBOOK  := vendor/liquibook/src/book

.PHONY: help all check-vcpkg-root configure build test clean \
        preflight debug test-debug release test-release tsan test-tsan \
        bench regenerate check-generated check-liquibook check

.DEFAULT_GOAL := all

help:
	@echo "Targets (default preset: $(PRESET); override with PRESET=release|tsan):"
	@echo "  make preflight         build+test the DEBUG preset, exactly as CI does"
	@echo "                         (run before pushing; release+LTO hides link errors)"
	@echo "  make configure         cmake --preset \$$(PRESET)"
	@echo "  make build             configure, then cmake --build --preset \$$(PRESET)"
	@echo "  make test              build, then ctest --preset \$$(PRESET) --output-on-failure"
	@echo "  make debug|release|tsan            build that preset"
	@echo "  make test-debug|test-release|test-tsan"
	@echo "  make test TEST_FILTER=Matching     filter test names (ctest -R)"
	@echo ""
	@echo "  make bench             run bench/apply_benchmark, release preset (spec §9.1)"
	@echo "  make regenerate        run the vendored SBE tool over $(SCHEMA) into $(GENERATED)/"
	@echo "  make check-generated   fail if $(GENERATED)/ differs from a fresh generation (CI)"
	@echo "  make check-liquibook   fail if the vendored liquibook gained a forbidden construct (CI)"
	@echo "  make check             check-generated + check-liquibook"
	@echo ""
	@echo "  make clean             remove build/\$$(PRESET) (all presets if PRESET is unset)"

all: build

check-vcpkg-root:
	@if [ -z "$(VCPKG_ROOT)" ]; then \
		echo 'error: VCPKG_ROOT is not set -- export it, e.g. export VCPKG_ROOT=$$HOME/workspace/vcpkg' >&2; \
		exit 1; \
	fi

configure: check-vcpkg-root
	cmake --preset $(PRESET)

build: configure
	cmake --build --preset $(PRESET)

test: build
	ctest --preset $(PRESET) $(CTEST_FLAGS)

# What CI runs, run locally, before you push. CI builds DEBUG; fleet
# work builds RELEASE, and release links with LTO, which folds duplicate
# symbols that debug rejects (spec §10.5). Green here is the evidence.
preflight: check-vcpkg-root check
	@echo "== preflight: exactly what .github/workflows/ci.yml runs =="
	cmake --preset debug
	cmake --build --preset debug
	ctest --preset debug --output-on-failure
	@echo "== preflight passed =="

debug:
	$(MAKE) build PRESET=debug
test-debug:
	$(MAKE) test PRESET=debug
release:
	$(MAKE) build PRESET=release
test-release:
	$(MAKE) test PRESET=release
tsan:
	$(MAKE) build PRESET=tsan
test-tsan:
	$(MAKE) test PRESET=tsan

bench: release
	./build/release/bench/apply_benchmark

# SBE generation (spec §3.2). Generated headers are checked in; a JVM
# is needed only here. Generation goes to a scratch directory first so
# a failed run cannot leave generated/ half-written.
SBE_GEN = java -Dsbe.target.language=cpp -Dsbe.output.dir=$(1) -Dsbe.xinclude.aware=true \
          -Dsbe.validation.stop.on.error=true -Dsbe.validation.warnings.fatal=true \
          -jar $(SBE_JAR) $(SCHEMA)

regenerate:
	@(cd vendor/sbe && sha256sum -c sbe-all-1.40.1.jar.sha256 >/dev/null)
	@tmp=$$(mktemp -d) && $(call SBE_GEN,$$tmp) && rm -rf $(GENERATED) && mv $$tmp $(GENERATED) && \
		echo "regenerated $(GENERATED)/ from $(SCHEMA)"

check-generated:
	@(cd vendor/sbe && sha256sum -c sbe-all-1.40.1.jar.sha256 >/dev/null)
	@tmp=$$(mktemp -d) && $(call SBE_GEN,$$tmp) && \
		if diff -r $$tmp $(GENERATED) >/dev/null; then rm -rf $$tmp; echo "check-generated: $(GENERATED)/ is current"; \
		else diff -r $$tmp $(GENERATED) | head -40; rm -rf $$tmp; \
		     echo "error: $(GENERATED)/ differs from a fresh generation -- run 'make regenerate' and commit" >&2; exit 1; fi

# spec §5: the constructs a replicated state machine may not use, grepped
# by behaviour (spec §10.8) over the vendored copy. Two lists:
#   forbidden  -- any hit fails: floating point, clocks, randomness,
#                 unordered containers.
#   io         -- hits must equal vendor/liquibook/io-allowlist.txt, the
#                 known std::cerr lines on exception/warning paths. A new
#                 line is a review, so it fails until the allowlist is
#                 updated deliberately.
FORBIDDEN_RE := \bdouble\b|\bfloat\b|std::chrono|\btime\(|\bclock\(|\brand\(|\brandom_device|<random>|unordered_map|unordered_set|thread_local
IO_RE        := std::cerr|std::cout|printf|fopen|fstream|std::clog
check-liquibook:
	@if grep -nE '$(FORBIDDEN_RE)' $(LIQUIBOOK)/*.h; then \
		echo "error: forbidden construct in vendored liquibook (spec §5)" >&2; exit 1; fi
	@mkdir -p build && (grep -nE '$(IO_RE)' $(LIQUIBOOK)/*.h || true) > build/liquibook-io.txt
	@if ! diff -u vendor/liquibook/io-allowlist.txt build/liquibook-io.txt; then \
		echo "error: I/O sites in vendored liquibook changed -- review, then update vendor/liquibook/io-allowlist.txt" >&2; exit 1; fi
	@echo "check-liquibook: no forbidden constructs; I/O sites match the allowlist"

check: check-generated check-liquibook

clean:
ifeq ($(origin PRESET), command line)
	rm -rf build/$(PRESET)
else
	rm -rf build/debug build/release build/tsan
endif
