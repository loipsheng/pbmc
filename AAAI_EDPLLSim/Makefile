EDPLL_TARGET := EDPLLSim
SIM_TARGET := Sim

CXX := g++
CXXFLAGS := -Wall -Wextra -std=c++17 -O3 -g -MMD -MP
LDLIBS := -lgmp -lm -lglpk -L./lib -lpatoh -pthread

BUILD_DIR := build
EDPLL_BUILD := $(BUILD_DIR)/edpllsim
SIM_BUILD := $(BUILD_DIR)/sim
TEST_BUILD := $(BUILD_DIR)/tests
ROW_NORMALIZER_TEST := $(TEST_BUILD)/row_normalizer_test
CACHE_KEY_TEST := $(TEST_BUILD)/cache_key_test
RIR_LP_TEST := $(TEST_BUILD)/rir_lp_test
ADAPTIVE_K_TEST := $(TEST_BUILD)/adaptive_k_test

EDPLL_SRCS := $(wildcard src/*.cc)
EDPLL_OBJS := $(patsubst src/%.cc,$(EDPLL_BUILD)/%.o,$(EDPLL_SRCS))

SIM_OBJS := \
	$(SIM_BUILD)/sim_main.o \
	$(SIM_BUILD)/sim.o \
	$(SIM_BUILD)/linear_constraint.o \
	$(SIM_BUILD)/big_int.o \
	$(SIM_BUILD)/cache.o \
	$(SIM_BUILD)/heuristic.o

ALL_DEP_OBJS := $(sort $(EDPLL_OBJS) $(SIM_OBJS) \
	$(TEST_BUILD)/row_normalizer_test.o $(TEST_BUILD)/cache_key_test.o \
	$(TEST_BUILD)/rir_lp_test.o $(TEST_BUILD)/adaptive_k_test.o)
-include $(ALL_DEP_OBJS:.o=.d)

all: $(EDPLL_TARGET) $(SIM_TARGET)

$(EDPLL_TARGET): $(EDPLL_OBJS)
	$(CXX) -o $@ $^ $(LDLIBS)

$(SIM_TARGET): $(SIM_OBJS)
	$(CXX) -o $@ $^ $(LDLIBS)

$(EDPLL_BUILD) $(SIM_BUILD):
	mkdir -p $@

$(TEST_BUILD):
	mkdir -p $@

$(EDPLL_BUILD)/%.o: src/%.cc | $(EDPLL_BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SIM_BUILD)/sim_main.o: sim/src/main.cc | $(SIM_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(SIM_BUILD)/sim.o: sim/src/sim.cc | $(SIM_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(SIM_BUILD)/linear_constraint.o: src/linear_constraint.cc | $(SIM_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(SIM_BUILD)/big_int.o: src/big_int.cc | $(SIM_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(SIM_BUILD)/cache.o: src/cache.cc | $(SIM_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(SIM_BUILD)/heuristic.o: src/heuristic.cc | $(SIM_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(TEST_BUILD)/row_normalizer_test.o: tests/row_normalization/row_normalizer_test.cc | $(TEST_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(ROW_NORMALIZER_TEST): \
	$(TEST_BUILD)/row_normalizer_test.o \
	$(EDPLL_BUILD)/big_int.o \
	$(EDPLL_BUILD)/cache.o \
	$(EDPLL_BUILD)/heuristic.o \
	$(EDPLL_BUILD)/linear_constraint.o
	$(CXX) -o $@ $^ $(LDLIBS)

test-row-normalizer: $(ROW_NORMALIZER_TEST)
	./$(ROW_NORMALIZER_TEST)

$(TEST_BUILD)/cache_key_test.o: tests/cache_key/cache_key_test.cc | $(TEST_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(CACHE_KEY_TEST): \
	$(TEST_BUILD)/cache_key_test.o \
	$(EDPLL_BUILD)/big_int.o \
	$(EDPLL_BUILD)/cache.o \
	$(EDPLL_BUILD)/heuristic.o \
	$(EDPLL_BUILD)/linear_constraint.o \
	$(EDPLL_BUILD)/solver.o
	$(CXX) -o $@ $^ $(LDLIBS)

test-cache-key: $(CACHE_KEY_TEST)
	./$(CACHE_KEY_TEST)

$(TEST_BUILD)/rir_lp_test.o: tests/rir_lp/rir_lp_test.cc | $(TEST_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(RIR_LP_TEST): \
	$(TEST_BUILD)/rir_lp_test.o \
	$(EDPLL_BUILD)/big_int.o \
	$(EDPLL_BUILD)/cache.o \
	$(EDPLL_BUILD)/heuristic.o \
	$(EDPLL_BUILD)/linear_constraint.o \
	$(EDPLL_BUILD)/solver.o
	$(CXX) -o $@ $^ $(LDLIBS)

test-rir-lp: $(RIR_LP_TEST)
	./$(RIR_LP_TEST)

$(TEST_BUILD)/adaptive_k_test.o: tests/adaptive_k/adaptive_k_test.cc | $(TEST_BUILD)
	$(CXX) $(CXXFLAGS) -I./src -c $< -o $@

$(ADAPTIVE_K_TEST): \
	$(TEST_BUILD)/adaptive_k_test.o \
	$(EDPLL_BUILD)/adaptive_k.o \
	$(EDPLL_BUILD)/big_int.o \
	$(EDPLL_BUILD)/cache.o \
	$(EDPLL_BUILD)/heuristic.o \
	$(EDPLL_BUILD)/linear_constraint.o
	$(CXX) -o $@ $^ $(LDLIBS)

test-adaptive-k: $(ADAPTIVE_K_TEST)
	./$(ADAPTIVE_K_TEST)

clean:
	rm -rf $(BUILD_DIR) $(EDPLL_TARGET) $(SIM_TARGET)

clean-EDPLLSim:
	rm -rf $(EDPLL_BUILD) $(EDPLL_TARGET)

clean-Sim:
	rm -rf $(SIM_BUILD) $(SIM_TARGET)

.PHONY: all clean clean-EDPLLSim clean-Sim test-row-normalizer test-cache-key test-rir-lp test-adaptive-k
