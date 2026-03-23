PROJECT_NAME := dash
BUILD_DIR := build
BINARY := $(BUILD_DIR)/$(PROJECT_NAME)

CXX ?= clang++
AR ?= ar
LLVM_CONFIG ?= $(shell command -v llvm-config-22 2>/dev/null || command -v llvm-config 2>/dev/null)

ifeq ($(strip $(LLVM_CONFIG)),)
$(error llvm-config was not found. Install LLVM and rerun with LLVM_CONFIG=/path/to/llvm-config)
endif

SRC := \
    src/main.cpp \
    src/frontend/lexer.cpp \
    src/frontend/parser.cpp \
    src/frontend/source_loader.cpp \
    src/sema/analyzer.cpp \
    src/codegen/codegen.cpp

OBJ := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

BASE_CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -MMD -MP -Iinclude
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --system-libs --libs all)

CXXFLAGS := $(BASE_CXXFLAGS) $(filter-out -std=% -fno-exceptions,$(LLVM_CXXFLAGS)) -fexceptions
LDFLAGS := $(LLVM_LDFLAGS)

.PHONY: all clean test samples dirs

all: $(BINARY)

dirs:
	@mkdir -p $(BUILD_DIR) $(BUILD_DIR)/frontend $(BUILD_DIR)/sema $(BUILD_DIR)/codegen

$(BINARY): dirs $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

test: $(BINARY)
	@mkdir -p $(BUILD_DIR)/tests
	./$(BINARY) tests/smoke/basic_arithmetic.ds --emit-llvm -o $(BUILD_DIR)/tests/basic_arithmetic.ll
	./$(BINARY) tests/smoke/extern_printf.ds -c -o $(BUILD_DIR)/tests/extern_printf.o
	./$(BINARY) tests/smoke/hex_uint.ds --emit-llvm -o $(BUILD_DIR)/tests/hex_uint.ll

samples: $(BINARY)
	@mkdir -p $(BUILD_DIR)/samples
	./$(BINARY) samples/hello.ds --emit-llvm -o $(BUILD_DIR)/samples/hello.ll
	./$(BINARY) samples/hex_uint.ds --emit-llvm -o $(BUILD_DIR)/samples/hex_uint.ll

-include $(DEP)
