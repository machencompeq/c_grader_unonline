#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "executor.h"

#define COMPILE_TIMEOUT_MS 60000
#define COMPILE_LOG_LIMIT (1024 * 1024)

int compile_sources(const char *gcc_path, const char *flags, const StringList *sources,
                    const char *exe_path, const char *work_dir, CompileResult *result)
{
    StrBuf cmd = {0};
    char log_path[GRADER_PATH_MAX];
    RunLimits limits;
    RunResult run;
    int i;

    result->ok = 0;
    result->log = NULL;

    /* 同一個 exe 路徑可能會重試好幾次 (Big5、多檔案、AI 修正)：先刪掉舊的，成功與否才看得準 */
    delete_file(exe_path);

    /* "gcc" "a.c" "b.c" -o "prog.exe" -fdiagnostics-color=never -lm */
    sb_appendf(&cmd, "\"%s\"", gcc_path);
    for (i = 0; i < sources->count; i++)
        sb_appendf(&cmd, " \"%s\"", sources->items[i]);
    sb_appendf(&cmd, " -o \"%s\" -fdiagnostics-color=never", exe_path);
    if (flags != NULL && flags[0] != '\0')
        sb_appendf(&cmd, " %s", flags);

    path_join(log_path, sizeof(log_path), work_dir, "compile.log");

    limits.timeout_ms = COMPILE_TIMEOUT_MS;
    limits.output_limit = COMPILE_LOG_LIMIT;
    limits.memory_limit = 0;
    run_program(cmd.data, work_dir, NULL, log_path, 1, &limits, &run);

    result->log = read_file(log_path, NULL);
    if (result->log == NULL)
        result->log = _strdup("");

    if (run.status == RUN_START_FAILED) {
        StrBuf msg = {0};
        sb_appendf(&msg, "無法執行編譯器：%s\n請確認已安裝 gcc (MinGW-w64) 並加入 PATH。\n", gcc_path);
        free(result->log);
        result->log = msg.data;
    } else if (run.status == RUN_TIMEOUT) {
        StrBuf msg = {0};
        sb_append(&msg, result->log);
        sb_append(&msg, "\n[批改工具] 編譯超過時間限制，已中止。\n");
        free(result->log);
        result->log = msg.data;
    }

    result->ok = run.status == RUN_OK && run.exit_code == 0 && file_exists(exe_path);
    sb_free(&cmd);
    return result->ok;
}

void compile_result_free(CompileResult *result)
{
    free(result->log);
    result->log = NULL;
}
