# C 作業批改工具 - 用 MinGW-w64 的 mingw32-make 編譯
#   mingw32-make          編譯 Grader.exe (視覺化介面) 與 build/grader_cli.exe (命令列)
#   mingw32-make test     編譯並執行核心測試
#   mingw32-make clean    刪除 build/ 裡的編譯產物
#
# 老師只需要雙擊最上層的 Grader.exe；其他編譯產物都放在 build/。
# gcc 不在 PATH 時可以指定：mingw32-make CC=C:/mingw64/bin/gcc.exe WINDRES=C:/mingw64/bin/windres.exe

CC = gcc
WINDRES = windres
CFLAGS = -std=gnu11 -Wall -Wextra -O2 -DUNICODE -D_UNICODE

CORE = src/util.c src/executor.c src/compiler.c src/comparator.c src/scorer.c \
       src/config.c src/testcase.c src/grader.c src/exporter.c src/report.c src/roster.c src/ai_fix.c
GUI = src/main.c src/gui.c src/gui_common.c src/gui_rules.c src/gui_detail.c src/gui_tests.c
HEADERS = $(wildcard src/*.h)

all: Grader.exe build/grader_cli.exe

build/.dir:
	-mkdir build
	echo dir > build/.dir

Grader.exe: $(GUI) $(CORE) $(HEADERS) build/app_res.o
	$(CC) $(CFLAGS) -municode -mwindows -o $@ $(GUI) $(CORE) build/app_res.o \
		-lcomctl32 -lcomdlg32 -lshell32 -lole32 -luxtheme -ldwmapi -lgdi32

build/app_res.o: src/res/app.rc src/res/app.manifest src/res/app.ico build/.dir
	$(WINDRES) -i src/res/app.rc -o $@ --include-dir src/res

build/grader_cli.exe: src/cli_main.c $(CORE) $(HEADERS) build/.dir
	$(CC) $(CFLAGS) -o $@ src/cli_main.c $(CORE) -lshell32

build/test_core.exe: tests/test_core.c $(CORE) $(HEADERS) build/.dir
	$(CC) $(CFLAGS) -o $@ tests/test_core.c $(CORE) -lshell32

test: build/test_core.exe
	build/test_core.exe

clean:
	-cmd /c "del /q build\app_res.o build\grader_cli.exe build\test_core.exe build\Grader_next.exe"

.PHONY: all test clean
