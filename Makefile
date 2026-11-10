.PHONY: build test clean

# 本仓已冻结：实现与构建都在 kvspace 仓。
build:
	@echo "kvspace-c 已冻结，实现见 kvspace/src/shm。" >&2
	@echo "构建请用: cmake --build <kvspace-build> --target kvspace-c" >&2
	@exit 1

test: build

clean:
	@echo "无构建产物可清理（已冻结）"
