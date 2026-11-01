.PHONY: build test clean

build:
	@mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$$(nproc)

# Tests need the kvspace ABI headers (<kvspace/kvspace.h>) on the include path.
test:
	@mkdir -p build-test && cd build-test && \
		cmake -DCMAKE_BUILD_TYPE=Debug -DKVSPACE_BUILD_TESTS=ON .. && \
		make -j$$(nproc) && ctest --output-on-failure

clean:
	rm -rf build build-test
