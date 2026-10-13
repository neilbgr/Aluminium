RACK_DIR ?= ../..

FLAGS +=
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += presets
DISTRIBUTABLES += $(wildcard LICENSE*)

# `make test` needs no Rack SDK at all (see below).
ifneq ($(MAKECMDGOALS),test)
include $(RACK_DIR)/plugin.mk
endif

# Headless unit test for Zones' note tracking (src/ZonesTracker.hpp) — plain
# C++, no Rack engine. TEST_CXX defaults to the host compiler; to cross-build
# and run it under Wine instead (e.g. WSL without a native g++):
#   make test TEST_CXX=x86_64-w64-mingw32-g++ TEST_RUN=wine TEST_EXT=.exe
TEST_CXX ?= c++
TEST_RUN ?=
TEST_EXT ?=
TEST_BIN := build/zones-tracker-test$(TEST_EXT)

test:
	@mkdir -p build
	$(TEST_CXX) -std=c++11 -Wall -Wextra -Werror -O1 -Isrc tests/ZonesTrackerTest.cpp -o $(TEST_BIN) $(if $(filter .exe,$(TEST_EXT)),-static)
	$(TEST_RUN) ./$(TEST_BIN)

.PHONY: test
