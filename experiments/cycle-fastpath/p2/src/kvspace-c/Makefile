.PHONY: build clean

build:
	@mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$$(nproc)

clean:
	rm -rf build
