CXX = g++
CXX_STD = c++17
OPT_FLAGS ?= -O3
CXX_FLAGS = -std=$(CXX_STD) $(OPT_FLAGS) -Wall -Wextra -I./include -I./third_party
SRC := $(sort $(shell find ./src -type f -name '*.cpp' 2>/dev/null))
HEADERS := $(sort $(shell find ./include -type f -name '*.hpp' 2>/dev/null))
CIRCUIT_TEST_SOURCES = $(sort $(wildcard ./src/circuit/*.cpp)) \
	./src/devices/device.cpp
CONFIG_SOURCES = $(wildcard ./src/config/*.cpp)
CORE_TEST_SOURCES = ./src/app/command_line.cpp \
	./src/config/cli_option_overrides.cpp
TARGET = spice
BUILD_DIR ?= build
OBJECTS = $(patsubst ./src/%.cpp,$(BUILD_DIR)/%.o,$(SRC))
DEPENDENCIES = $(OBJECTS:.o=.d)
UNIT_TEST_SOURCE ?= tests/unit/transient_analysis_test.cpp
UNIT_TEST_BUILD_DIR ?= tests/.build
UNIT_TEST_TARGET ?= $(UNIT_TEST_BUILD_DIR)/transient_analysis_test
CORE_TEST_SOURCE ?= tests/unit/core_test.cpp
CORE_TEST_TARGET ?= $(UNIT_TEST_BUILD_DIR)/core_test
MOS3_DC_TEST_SOURCE ?= tests/unit/mos3_dc_test.cpp
MOS3_DC_TEST_TARGET ?= $(UNIT_TEST_BUILD_DIR)/mos3_dc_test
TESTCASE_ROOT ?= tests/cases
TEST_ROOT ?= tests
OP_TESTCASE_DIR ?= $(TESTCASE_ROOT)/op
TRAN_TESTCASE_DIR ?= $(TESTCASE_ROOT)/tran
PRIVATE_TESTCASE_DIR ?= tests/private
ACTUAL_DIR ?= tests/output
OP_ACTUAL_DIR ?= $(ACTUAL_DIR)/op
TRAN_ACTUAL_DIR ?= $(ACTUAL_DIR)/tran
PRIVATE_ACTUAL_DIR ?= $(ACTUAL_DIR)/private
PTA_OUTPUT_ROOT ?= $(ACTUAL_DIR)/pta
PTA_MODE ?= disabled
PTA_OUTPUT_DIR ?= $(PTA_OUTPUT_ROOT)/$(PTA_MODE)
PTA_HARD_OUTPUT_DIR ?= $(PTA_OUTPUT_ROOT)/hard-op
STANDARD_ROOT ?= tests/references
OP_STANDARD_DIR ?= $(STANDARD_ROOT)/op
TRAN_STANDARD_DIR ?= $(STANDARD_ROOT)/tran
TEST_SCRIPT_DIR ?= tests/scripts
CASE_RUNNER ?= $(TEST_SCRIPT_DIR)/run_cases.py
PYTHON ?= python3
OP_ABS_TOL ?= 5e-4
OP_REL_TOL ?= 1e-4
TRAN_ABS_TOL ?= 1e-4
TRAN_REL_TOL ?= 1e-3
TIME_ABS_TOL ?= 1e-15
OP_COMPARE_FLAGS ?=
TRAN_COMPARE_FLAGS ?=
PTA_COMPARE_FLAGS ?= $(OP_COMPARE_FLAGS)
NETLIST_PARSE_TIMEOUT ?= 120
MOS3_CASE_ROOT ?= $(TESTCASE_ROOT)/mos3
MOS3_REFERENCE_ROOT ?= $(STANDARD_ROOT)/mos3
MOS3_ACTUAL_ROOT ?= $(ACTUAL_DIR)/mos3
MOS3_OP_CASE_DIR ?= $(MOS3_CASE_ROOT)/op
MOS3_TRAN_CASE_DIR ?= $(MOS3_CASE_ROOT)/tran
MOS3_OP_STANDARD_DIR ?= $(MOS3_REFERENCE_ROOT)/op
MOS3_TRAN_STANDARD_DIR ?= $(MOS3_REFERENCE_ROOT)/tran
MOS3_OP_ACTUAL_DIR ?= $(MOS3_ACTUAL_ROOT)/op
MOS3_TRAN_ACTUAL_DIR ?= $(MOS3_ACTUAL_ROOT)/tran
MOS3_OP_ABS_TOL ?= 1e-7
MOS3_OP_REL_TOL ?= 2e-3
MOS3_TRAN_ABS_TOL ?= 1e-7
MOS3_TRAN_REL_TOL ?= 2e-3

UNAME_S := $(shell uname -s)

# Override: make EIGEN_INCLUDE=/path/to/eigen3
EIGEN_INCLUDE ?=

# pkg-config (typical on Linux; also works if eigen3.pc is installed elsewhere)
EIGEN_PKG_CFLAGS := $(shell pkg-config --cflags eigen3 2>/dev/null)

# Homebrew (macOS / Linuxbrew)
BREW_EIGEN_PREFIX := $(shell brew --prefix eigen 2>/dev/null)

# Search order: user path > pkg-config dirs > brew > common install locations
EIGEN_CANDIDATES :=
ifneq ($(strip $(EIGEN_INCLUDE)),)
EIGEN_CANDIDATES += $(EIGEN_INCLUDE)
endif
ifneq ($(strip $(EIGEN_PKG_CFLAGS)),)
EIGEN_CANDIDATES += $(patsubst -I%,%,$(filter -I%,$(EIGEN_PKG_CFLAGS)))
endif
ifneq ($(strip $(BREW_EIGEN_PREFIX)),)
EIGEN_CANDIDATES += $(BREW_EIGEN_PREFIX)/include/eigen3
endif
EIGEN_CANDIDATES += \
	/opt/homebrew/include/eigen3 \
	/usr/local/include/eigen3 \
	/usr/include/eigen3

EIGEN_DIR := $(firstword $(foreach d,$(EIGEN_CANDIDATES),$(if $(wildcard $(d)/Eigen/Core),$(d),)))

ifneq ($(EIGEN_DIR),)
EIGEN_FLAGS := -I$(EIGEN_DIR)
else ifneq ($(strip $(EIGEN_PKG_CFLAGS)),)
EIGEN_FLAGS := $(EIGEN_PKG_CFLAGS)
endif

.PHONY: all clean test test-unit test-core test-config test-io test-cases test-op test-tran test-netlists test-private test-pta-hard-op test-mos3 test-mos3-op test-mos3-tran test-mos3-dc test-mos3-dc-unit compare compare-op \
	compare-tran compare-mos3-op compare-mos3-tran generate-standards generate-mos3-standards check-eigen check-deps pta pta-run pta-accuracy \
	pta-force-standard pta-force-disabled pta-fallback-standard

all: $(TARGET)

check-eigen:
	@if [ -z "$(EIGEN_FLAGS)" ]; then \
		echo "Error: Eigen3 headers not found."; \
		echo ""; \
		case "$(UNAME_S)" in \
			Darwin) \
				echo "  macOS:  brew install eigen"; \
				echo "  Or:     make EIGEN_INCLUDE=/path/to/eigen3" ;; \
			Linux) \
				echo "  Debian/Ubuntu:  sudo apt install libeigen3-dev"; \
				echo "  Fedora/RHEL:    sudo dnf install eigen3-devel"; \
				echo "  Arch:           sudo pacman -S eigen"; \
				echo "  Or:             make EIGEN_INCLUDE=/path/to/eigen3" ;; \
			MINGW*|MSYS*|CYGWIN*) \
				echo "  MSYS2:  pacman -S mingw-w64-x86_64-eigen"; \
				echo "  Or:     make EIGEN_INCLUDE=/path/to/eigen3" ;; \
			*) \
				echo "  Set:    make EIGEN_INCLUDE=/path/to/eigen3" ;; \
		esac; \
		exit 1; \
	fi
	@echo "Eigen OK ($(UNAME_S)): $(if $(EIGEN_DIR),$(EIGEN_DIR),$(strip $(EIGEN_PKG_CFLAGS)))"
	@echo '#include <Eigen/Core>' | \
		$(CXX) -std=$(CXX_STD) $(EIGEN_FLAGS) -x c++ - -c -o /dev/null || \
		(echo "Error: Eigen headers found but compile test failed."; exit 1)

check-deps: check-eigen

$(TARGET): $(OBJECTS) | check-eigen
	$(CXX) $(CXX_FLAGS) $(EIGEN_FLAGS) -o $(TARGET) $(OBJECTS)

$(BUILD_DIR)/%.o: ./src/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXX_FLAGS) $(EIGEN_FLAGS) -MMD -MP -c -o "$@" "$<"

-include $(DEPENDENCIES)

$(UNIT_TEST_TARGET): $(UNIT_TEST_SOURCE) $(CIRCUIT_TEST_SOURCES) \
	$(HEADERS) | check-eigen
	@mkdir -p "$(@D)"
	$(CXX) $(CXX_FLAGS) $(EIGEN_FLAGS) -o "$@" "$<" \
		$(CIRCUIT_TEST_SOURCES)

test-unit: $(UNIT_TEST_TARGET)
	@"$(UNIT_TEST_TARGET)"

$(CORE_TEST_TARGET): $(CORE_TEST_SOURCE) $(CORE_TEST_SOURCES) \
	$(HEADERS) | check-eigen
	@mkdir -p "$(@D)"
	$(CXX) $(CXX_FLAGS) $(EIGEN_FLAGS) -o "$@" "$<" \
		$(CORE_TEST_SOURCES)

test-core: $(CORE_TEST_TARGET)
	@"$(CORE_TEST_TARGET)"

$(MOS3_DC_TEST_TARGET): $(MOS3_DC_TEST_SOURCE) $(HEADERS) | check-eigen
	@mkdir -p "$(@D)"
	$(CXX) $(CXX_FLAGS) $(EIGEN_FLAGS) -o "$@" "$<"

test-mos3-dc-unit: $(MOS3_DC_TEST_TARGET)
	@"$(MOS3_DC_TEST_TARGET)"

CONFIG_TEST_SOURCE ?= tests/unit/config_test.cpp
CONFIG_TEST_TARGET ?= $(UNIT_TEST_BUILD_DIR)/config_test
CONFIG_CLI_TEST_SCRIPT ?= $(TEST_SCRIPT_DIR)/test_config.py

$(CONFIG_TEST_TARGET): $(CONFIG_TEST_SOURCE) $(CONFIG_SOURCES) \
	$(HEADERS) | check-eigen
	@mkdir -p "$(@D)"
	$(CXX) $(CXX_FLAGS) $(EIGEN_FLAGS) -o "$@" "$<" \
		$(CONFIG_SOURCES)

test-config: $(CONFIG_TEST_TARGET) $(TARGET)
	@"$(CONFIG_TEST_TARGET)"
	@$(PYTHON) "$(CONFIG_CLI_TEST_SCRIPT)" ./$(TARGET)

test: $(TARGET) $(UNIT_TEST_TARGET) $(CORE_TEST_TARGET) $(CONFIG_TEST_TARGET) $(MOS3_DC_TEST_TARGET)
	@status=0; \
	"$(UNIT_TEST_TARGET)" || status=1; \
	"$(CORE_TEST_TARGET)" || status=1; \
	"$(MOS3_DC_TEST_TARGET)" || status=1; \
	$(MAKE) --no-print-directory test-config || status=1; \
	$(MAKE) --no-print-directory test-io || status=1; \
	$(MAKE) --no-print-directory test-op || status=1; \
	$(MAKE) --no-print-directory test-tran || status=1; \
	$(MAKE) --no-print-directory test-mos3 || status=1; \
	$(MAKE) --no-print-directory test-netlists || status=1; \
	exit $$status

test-io: $(TARGET)
	@$(PYTHON) $(TEST_SCRIPT_DIR)/test_io.py ./$(TARGET)

test-cases:
	@$(PYTHON) $(TEST_SCRIPT_DIR)/check_case_complexity.py

test-op: $(TARGET)
	@rm -rf "$(OP_ACTUAL_DIR)"; \
	mkdir -p "$(OP_ACTUAL_DIR)"; \
	status=0; \
	$(PYTHON) "$(CASE_RUNNER)" \
		--analysis op \
		--simulator "./$(TARGET)" \
		--case-dir "$(OP_TESTCASE_DIR)" \
		--output-dir "$(OP_ACTUAL_DIR)" || status=1; \
	$(PYTHON) $(TEST_SCRIPT_DIR)/validate_raw.py \
		--analysis op \
		--listing-dir "$(OP_ACTUAL_DIR)" \
		"$(OP_ACTUAL_DIR)"/*/*.raw || status=1; \
	$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis op \
		--standard "$(OP_STANDARD_DIR)" \
		--actual "$(OP_ACTUAL_DIR)" \
		--atol "$(OP_ABS_TOL)" \
		--rtol "$(OP_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)" \
		$(OP_COMPARE_FLAGS) || status=1; \
	exit $$status

test-tran: $(TARGET)
	@rm -rf "$(TRAN_ACTUAL_DIR)"; \
	mkdir -p "$(TRAN_ACTUAL_DIR)"; \
	status=0; \
	$(PYTHON) "$(CASE_RUNNER)" \
		--analysis tran \
		--simulator "./$(TARGET)" \
		--case-dir "$(TRAN_TESTCASE_DIR)" \
		--output-dir "$(TRAN_ACTUAL_DIR)" || status=1; \
	$(PYTHON) $(TEST_SCRIPT_DIR)/validate_raw.py \
		--analysis tran \
		--listing-dir "$(TRAN_ACTUAL_DIR)" \
		"$(TRAN_ACTUAL_DIR)"/*/*.raw || status=1; \
	$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis tran \
		--standard "$(TRAN_STANDARD_DIR)" \
		--actual "$(TRAN_ACTUAL_DIR)" \
		--atol "$(TRAN_ABS_TOL)" \
		--rtol "$(TRAN_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)" \
		$(TRAN_COMPARE_FLAGS) || status=1; \
	exit $$status

# Keep focused MOS3 targets for local development; the complete MOS3 suite is
# also part of the default `test` gate.
test-mos3: test-mos3-op test-mos3-tran

test-mos3-dc: test-mos3-dc-unit test-mos3-op

test-mos3-op: $(TARGET)
	@rm -rf "$(MOS3_OP_ACTUAL_DIR)"; \
	mkdir -p "$(MOS3_OP_ACTUAL_DIR)"; \
	status=0; \
	$(PYTHON) "$(CASE_RUNNER)" \
		--analysis op \
		--simulator "./$(TARGET)" \
		--case-dir "$(MOS3_OP_CASE_DIR)" \
		--include mos3_01_cmosedu_nmos_bias \
		--include mos3_02_cmosedu_pmos_bias \
		--include mos3_03_mos6_process_model_op \
		--include mos3_04_bug481_series_geometry_op \
		--output-dir "$(MOS3_OP_ACTUAL_DIR)" || status=1; \
	$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis op \
		--standard "$(MOS3_OP_STANDARD_DIR)" \
		--actual "$(MOS3_OP_ACTUAL_DIR)" \
		--atol "$(MOS3_OP_ABS_TOL)" \
		--rtol "$(MOS3_OP_REL_TOL)" \
		--include mos3_01_cmosedu_nmos_bias \
		--include mos3_02_cmosedu_pmos_bias \
		--include mos3_03_mos6_process_model_op \
		--include mos3_04_bug481_series_geometry_op \
		--time-atol "$(TIME_ABS_TOL)" || status=1; \
	exit $$status

test-mos3-tran: $(TARGET)
	@rm -rf "$(MOS3_TRAN_ACTUAL_DIR)"; \
	mkdir -p "$(MOS3_TRAN_ACTUAL_DIR)"; \
	status=0; \
	$(PYTHON) "$(CASE_RUNNER)" \
		--analysis tran \
		--simulator "./$(TARGET)" \
		--case-dir "$(MOS3_TRAN_CASE_DIR)" \
		--output-dir "$(MOS3_TRAN_ACTUAL_DIR)" || status=1; \
	$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis tran \
		--standard "$(MOS3_TRAN_STANDARD_DIR)" \
		--actual "$(MOS3_TRAN_ACTUAL_DIR)" \
		--atol "$(MOS3_TRAN_ABS_TOL)" \
		--rtol "$(MOS3_TRAN_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)" || status=1; \
	exit $$status

test-private: $(TARGET)
	@rm -rf "$(PRIVATE_ACTUAL_DIR)"; \
	mkdir -p "$(PRIVATE_ACTUAL_DIR)"; \
	$(PYTHON) "$(CASE_RUNNER)" \
		--analysis private \
		--simulator "./$(TARGET)" \
		--case-dir "$(PRIVATE_TESTCASE_DIR)" \
		--output-dir "$(PRIVATE_ACTUAL_DIR)"

test-netlists: $(TARGET)
	@$(PYTHON) $(TEST_SCRIPT_DIR)/test_netlist_parsing.py \
		./$(TARGET) \
		$(TEST_ROOT) \
		--recursive \
		--timeout $(NETLIST_PARSE_TIMEOUT)

# This is a legacy-Newton/PTA differentiation benchmark, not a permanent
# release gate.  The script disables trust-region explicitly for its ordinary
# and fallback branches so it remains a stable PTA recovery fixture.
test-pta-hard-op: $(TARGET)
	@rm -rf "$(PTA_HARD_OUTPUT_DIR)"; \
	mkdir -p "$(PTA_HARD_OUTPUT_DIR)"; \
	$(PYTHON) $(TEST_SCRIPT_DIR)/test_pta_hard_op.py \
		./$(TARGET) \
		$(TESTCASE_ROOT)/pta/nr_fail_cross_coupled_cmos_latch.cir \
		$(STANDARD_ROOT)/pta/nr_fail_cross_coupled_cmos_latch.out \
		--output-root "$(PTA_HARD_OUTPUT_DIR)"

# PTA tests intentionally use only OP decks.  Every mode is compared against
# the existing ngspice OP references; pta-run prints the end-to-end suite time.
# PTA is implemented as an experimental solver path.  Until its adaptive
# capacitor control and scaled derivative convergence test are complete,
# Force failures remain useful diagnostics rather than release regressions.
pta-run: $(TARGET)
	@case "$(PTA_MODE)" in \
		disabled|force|fallback) ;; \
		*) echo "Invalid PTA_MODE <$(PTA_MODE)>; expected disabled, force, or fallback"; exit 2 ;; \
	esac; \
	rm -rf "$(PTA_OUTPUT_DIR)"; \
	mkdir -p "$(PTA_OUTPUT_DIR)"; \
	start=$$($(PYTHON) -c 'import time; print(time.perf_counter())'); \
	status=0; count=0; \
	for netlist in "$(OP_TESTCASE_DIR)"/*.cir; do \
		if [ ! -f "$$netlist" ]; then \
			echo "No OP cases found in $(OP_TESTCASE_DIR)"; status=1; break; \
		fi; \
		name=$${netlist##*/}; name=$${name%.cir}; \
		"./$(TARGET)" --pta "$(PTA_MODE)" -b \
			--output-root "$(PTA_OUTPUT_DIR)" "$$netlist" \
			>/dev/null || status=1; \
		count=$$((count + 1)); \
	done; \
	finish=$$($(PYTHON) -c 'import time; print(time.perf_counter())'); \
	$(PYTHON) -c 'import sys; print("TIME PTA {:<8} suite ({} cases) {:.3f} ms".format(sys.argv[3], sys.argv[4], (float(sys.argv[2]) - float(sys.argv[1])) * 1000.0))' \
		"$$start" "$$finish" "$(PTA_MODE)" "$$count"; \
	exit $$status

pta-accuracy:
	@$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis op \
		--standard "$(OP_STANDARD_DIR)" \
		--actual "$(PTA_OUTPUT_DIR)" \
		--atol "$(OP_ABS_TOL)" \
		--rtol "$(OP_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)" \
		$(PTA_COMPARE_FLAGS)

# Force versus standard: accuracy.  The comparator is still run when Force
# fails so missing listings and stderr diagnostics are reported explicitly.
pta-force-standard:
	@status=0; \
	$(MAKE) --no-print-directory pta-run PTA_MODE=force || status=1; \
	$(MAKE) --no-print-directory pta-accuracy PTA_MODE=force || status=1; \
	exit $$status

# Force versus Disabled: each mode reports its suite time and is independently
# checked against the same ngspice OP reference set for accuracy.
pta-force-disabled:
	@status=0; \
	$(MAKE) --no-print-directory pta-run PTA_MODE=disabled || status=1; \
	$(MAKE) --no-print-directory pta-accuracy PTA_MODE=disabled || status=1; \
	$(MAKE) --no-print-directory pta-run PTA_MODE=force || status=1; \
	$(MAKE) --no-print-directory pta-accuracy PTA_MODE=force || status=1; \
	exit $$status

# Fallback versus standard: accuracy.
pta-fallback-standard:
	@status=0; \
	$(MAKE) --no-print-directory pta-run PTA_MODE=fallback || status=1; \
	$(MAKE) --no-print-directory pta-accuracy PTA_MODE=fallback || status=1; \
	exit $$status

pta:
	@status=0; \
	$(MAKE) --no-print-directory pta-force-standard || status=1; \
	$(MAKE) --no-print-directory pta-force-disabled || status=1; \
	$(MAKE) --no-print-directory pta-fallback-standard || status=1; \
	exit $$status

compare: compare-op compare-tran

generate-standards: test-cases
	@$(PYTHON) $(TEST_SCRIPT_DIR)/generate_ngspice_standards.py

generate-mos3-standards:
	@$(PYTHON) $(TEST_SCRIPT_DIR)/generate_ngspice_standards.py --suite mos3

compare-op:
	@$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis op \
		--standard "$(OP_STANDARD_DIR)" \
		--actual "$(OP_ACTUAL_DIR)" \
		--atol "$(OP_ABS_TOL)" \
		--rtol "$(OP_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)" \
		$(OP_COMPARE_FLAGS)

compare-tran:
	@$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis tran \
		--standard "$(TRAN_STANDARD_DIR)" \
		--actual "$(TRAN_ACTUAL_DIR)" \
		--atol "$(TRAN_ABS_TOL)" \
		--rtol "$(TRAN_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)" \
		$(TRAN_COMPARE_FLAGS)

compare-mos3-op:
	@$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis op \
		--standard "$(MOS3_OP_STANDARD_DIR)" \
		--actual "$(MOS3_OP_ACTUAL_DIR)" \
		--atol "$(MOS3_OP_ABS_TOL)" \
		--rtol "$(MOS3_OP_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)"

compare-mos3-tran:
	@$(PYTHON) $(TEST_SCRIPT_DIR)/compare_spice.py \
		--analysis tran \
		--standard "$(MOS3_TRAN_STANDARD_DIR)" \
		--actual "$(MOS3_TRAN_ACTUAL_DIR)" \
		--atol "$(MOS3_TRAN_ABS_TOL)" \
		--rtol "$(MOS3_TRAN_REL_TOL)" \
		--time-atol "$(TIME_ABS_TOL)"

clean:
	rm -f $(TARGET)
	rm -rf "$(BUILD_DIR)"
	rm -rf "$(ACTUAL_DIR)"
	rm -rf "$(UNIT_TEST_BUILD_DIR)"
