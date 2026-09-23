/*
 * compiler.h - 把 C 原始碼編譯成 .exe (呼叫 gcc)。
 */
#ifndef COMPILER_H
#define COMPILER_H

#include "util.h"

typedef struct {
    int ok;     /* 1 = 編譯成功 */
    char *log;  /* gcc 的輸出 (錯誤與警告)，malloc 的記憶體 */
} CompileResult;

/*
 * gcc_path   : gcc 執行檔，例如 "gcc" 或 "C:\\mingw64\\bin\\gcc.exe"
 * flags      : 額外參數，例如 "-lm"
 * sources    : 要一起編譯的 .c 檔 (完整路徑)
 * exe_path   : 輸出的 .exe
 * work_dir   : 暫存資料夾 (compile.log 會放這裡)
 */
int compile_sources(const char *gcc_path, const char *flags, const StringList *sources,
                    const char *exe_path, const char *work_dir, CompileResult *result);

void compile_result_free(CompileResult *result);

#endif
