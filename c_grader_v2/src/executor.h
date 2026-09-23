/*
 * executor.h - 執行一個外部程式 (學生程式 / gcc)。
 *
 * 負責：
 *   - 把 testcase 檔案接到 stdin
 *   - 把 stdout 寫到檔案
 *   - Timeout：超過時間就強制結束
 *   - Output Size Limit：輸出太大就強制結束
 *   - 用 Windows Job Object 管理：批改工具結束時，學生程式 (含它開的子程式) 一定被關掉
 *
 * 注意：這不是安全沙盒。學生程式仍然可以讀寫檔案、連網路。
 * 只適合 TA 在自己電腦上批改，不能拿來當公開的線上評測系統。
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

typedef enum {
    RUN_OK,             /* 正常結束 */
    RUN_RUNTIME_ERROR,  /* 程式當掉 (例如 segmentation fault) */
    RUN_TIMEOUT,        /* 超過時間限制 */
    RUN_OUTPUT_LIMIT,   /* 輸出超過大小限制 */
    RUN_START_FAILED    /* 根本無法啟動 */
} RunStatus;

typedef struct {
    int timeout_ms;          /* 0 = 不限制 */
    long long output_limit;  /* bytes，0 = 不限制 */
    long long memory_limit;  /* bytes，0 = 不限制 */
} RunLimits;

typedef struct {
    RunStatus status;
    unsigned long exit_code;
    int elapsed_ms;
} RunResult;

/*
 * command_line : 完整命令列 (UTF-8)，例如 "\"C:\\temp\\prog.exe\""
 * working_dir  : 程式的工作目錄
 * stdin_path   : stdin 來源檔案，NULL 表示空輸入
 * stdout_path  : stdout 寫入的檔案，NULL 表示丟掉
 * merge_stderr : 1 = stderr 也寫進 stdout_path (編譯時用來收集錯誤訊息)
 *
 * 回傳 result->status。
 */
RunStatus run_program(const char *command_line, const char *working_dir,
                      const char *stdin_path, const char *stdout_path,
                      int merge_stderr, const RunLimits *limits, RunResult *result);

const char *run_status_name(RunStatus status);

/* 程式啟動時呼叫一次：學生程式當掉時不要跳出 Windows 錯誤對話框卡住批改 */
void executor_init(void);

#endif
