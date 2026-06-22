# Convenience wrapper around the CMake build. The canonical build system is
# CMake (see CMakeLists.txt); this just drives it so `make` works out of the box.

BUILD_DIR ?= build

.PHONY: all clean test test-opt

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j

clean:
	rm -rf $(BUILD_DIR)

test: all
	tests/run_tests.sh

test-opt: all
	tests/run_tests.sh -opt
