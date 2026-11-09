SHELL := /bin/bash

.DEFAULT_GOAL := all

INPUT ?=
ARGS ?=
SMOKE_INPUT ?= benchmarks/opb/knapsack/k-dim-knapsack-bench-1-104-4.opb

.PHONY: all release debug static clean rebuild setup run test help

all: release

release:
	./build.sh r

debug:
	./build.sh d

static:
	./build.sh rs

clean:
	./clean.sh

rebuild: clean release

setup:
	./setup-complete-code.sh

run: release
	@if [[ -z "$(INPUT)" ]]; then \
		echo '用法: make run INPUT=path/to/input.opb ARGS="可选参数"'; \
		exit 2; \
	fi
	./bin/pbmc $(ARGS) "$(INPUT)"

test: release
	@if [[ ! -f "$(SMOKE_INPUT)" ]]; then \
		echo "找不到冒烟测试输入: $(SMOKE_INPUT)"; \
		exit 2; \
	fi
	./bin/pbmc "$(SMOKE_INPUT)"

help:
	@echo 'PBMC 构建命令:'
	@echo '  make                 编译 Release 动态链接版本（等价于 make release）'
	@echo '  make release         编译 Release 动态链接版本到 bin/pbmc'
	@echo '  make debug           编译 Debug 版本到 bin-debug/pbmc'
	@echo '  make static          编译 Release 静态链接版本到 bin/pbmc'
	@echo '  make clean           删除 build/bin 和 build-debug/bin-debug'
	@echo '  make rebuild         清理后重新编译 Release 版本'
	@echo '  make setup           首次下载并拼装 GPMC/RoundingSat 完整源码'
	@echo '  make run INPUT=...   编译后运行一个 OPB 文件，可通过 ARGS 传参数'
	@echo '  make test            编译并运行一个背包实例作为冒烟测试'
	@echo '  make help            显示本帮助'
	@echo
	@echo '注意：make clean 不会运行 clean-up.sh，也不会删除源码或数据集。'
