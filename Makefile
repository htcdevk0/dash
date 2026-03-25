PROJECT_NAME := dash
BUILD_DIR := build
BINARY := $(BUILD_DIR)/$(PROJECT_NAME)

CXX ?= clang++

BUILD ?= release
LLVM_DIR ?= ./LLVM-22.1.1

SRC := $(shell find src -name '*.cpp')
OBJ := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

ifeq ($(BUILD),debug)
	OPT_FLAGS := -O0 -g
else
	OPT_FLAGS := -O2
endif

BASE_CXXFLAGS := -std=c++20 $(OPT_FLAGS) -Wall -Wextra -Wpedantic -MMD -MP -Iinclude

LLVM_CONFIG_LOCAL := $(LLVM_DIR)/bin/llvm-config
LLVM_CONFIG_SYS := $(shell command -v llvm-config)

ifeq ($(MODE),dynamic)
	LLVM_CONFIG := $(LLVM_CONFIG_SYS)
else
	LLVM_CONFIG := $(LLVM_CONFIG_LOCAL)
endif

ifeq ($(MODE),dynamic)
	LLVM_LINK := $(shell $(LLVM_CONFIG) --ldflags --libs --system-libs)
else
	LLVM_LINK := $(shell $(LLVM_CONFIG) --ldflags --libs --system-libs --link-static)
endif

LLVM_CXXFLAGS := $(filter-out -fno-rtti -fno-exceptions -std=%,$(shell $(LLVM_CONFIG) --cxxflags))

CXXFLAGS := $(BASE_CXXFLAGS) $(LLVM_CXXFLAGS) -fexceptions -frtti
LDFLAGS := $(LLVM_LINK) -static-libstdc++ -static-libgcc

.PHONY: all dynamic clean setup-llvm ensure-llvm build

all: ensure-llvm
	$(MAKE) MODE=static build

dynamic: ensure-llvm
	$(MAKE) MODE=dynamic build

setup-llvm:
	@chmod +x ./setup/llvm.sh
	@./setup/llvm.sh

ensure-llvm:
ifeq ($(MODE),dynamic)
	@if [ -z "$(LLVM_CONFIG_SYS)" ]; then \
		echo "[error] system llvm-config not found"; \
		exit 1; \
	fi
else
	@if [ ! -x "$(LLVM_CONFIG_LOCAL)" ]; then \
		printf "[setup] LLVM not found. Download now? [Y/n] "; \
		read ans; \
		if [ "$$ans" != "n" ] && [ "$$ans" != "N" ]; then \
			$(MAKE) setup-llvm; \
		else \
			echo "[error] LLVM required"; \
			exit 1; \
		fi \
	fi
endif

build: $(BINARY)

$(BINARY): $(OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEP)