# Host-side verification for the nabu_system loop-style disk.
#
# The kernel glue (NabuSystemDisk.cpp) needs the XNU 8796 headers; everything
# that decides *what bytes a given LBA returns* is IOKit-free and is compiled
# and checked here.
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
SRC      := src
BUILD    := build

all: $(BUILD)/test_disk_map $(BUILD)/test_disk_core

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_disk_map: tests/test_disk_map.cpp $(SRC)/NabuDiskMap.h $(SRC)/NabuGpt.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) $< -o $@

$(BUILD)/test_disk_core: tests/test_disk_core.cpp $(SRC)/NabuDiskCore.cpp \
                         $(SRC)/NabuDiskCore.h $(SRC)/NabuDiskMap.h $(SRC)/NabuGpt.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) tests/test_disk_core.cpp $(SRC)/NabuDiskCore.cpp -o $@

check: all
	python3 tools/gen_gpt.py --verify --dump $(BUILD)/reference.bin
	$(BUILD)/test_disk_map $(BUILD)/reference.bin
	$(BUILD)/test_disk_core $(BUILD)/reference.bin

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
