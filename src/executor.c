#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "executor.h"
#include "util.h"

void executor_init(void)
{
    /* 子程式會繼承這個設定：當掉時不跳 "程式已停止運作" 視窗 */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

const char *run_status_name(RunStatus status)
{
    switch (status) {
    case RUN_OK:            return "OK";
    case RUN_RUNTIME_ERROR: return "Runtime Error";
    case RUN_TIMEOUT:       return "Time Limit Exceeded";
    case RUN_OUTPUT_LIMIT:  return "Output Limit Exceeded";
    case RUN_START_FAILED:  return "Start Failed";
    }
    return "Unknown";
}

/* 開一個可以讓子程式繼承的檔案 handle。path == NULL 時開 NUL 裝置。 */
static HANDLE open_std_handle(const char *path, int for_write)
{
    SECURITY_ATTRIBUTES sa;
    wchar_t *w;
    HANDLE h;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (path == NULL)
        return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    w = utf8_to_wide(path);
    if (w == NULL)
        return INVALID_HANDLE_VALUE;
    if (for_write)
        h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    else
        h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, &sa,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    return h;
}

/*
 * Windows 上程式當掉時，結束碼會是 0xC0000005 (存取違規)、0xC00000FD (堆疊溢位) 這類
 * NTSTATUS 錯誤碼。一般 return 1 / exit(1) 不算 Runtime Error。
 */
static int is_crash_exit_code(unsigned long code)
{
    return code >= 0xC0000000UL && code <= 0xC0FFFFFFUL;
}

RunStatus run_program(const char *command_line, const char *working_dir,
                      const char *stdin_path, const char *stdout_path,
                      int merge_stderr, const RunLimits *limits, RunResult *result)
{
    HANDLE in = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE, err = INVALID_HANDLE_VALUE;
    HANDLE job = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info;
    wchar_t *wcmd = NULL, *wdir = NULL;
    ULONGLONG start;
    DWORD exit_code = 0;
    BOOL in_job = FALSE;

    memset(result, 0, sizeof(*result));
    result->status = RUN_START_FAILED;
    memset(&pi, 0, sizeof(pi));

    in = open_std_handle(stdin_path, 0);
    out = open_std_handle(stdout_path, 1);
    err = merge_stderr ? out : open_std_handle(NULL, 1);
    if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE)
        goto cleanup;

    /* Job Object：關掉 job handle 時，裡面所有程式都會被結束 */
    job = CreateJobObjectW(NULL, NULL);
    if (job == NULL)
        goto cleanup;
    memset(&job_info, 0, sizeof(job_info));
    job_info.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (limits->memory_limit > 0) {
        job_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        job_info.ProcessMemoryLimit = (SIZE_T)limits->memory_limit;
    }
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info));

    wcmd = utf8_to_wide(command_line);
    wdir = working_dir != NULL ? utf8_to_wide(working_dir) : NULL;
    if (wcmd == NULL)
        goto cleanup;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;

    /* 先暫停建立 -> 放進 job -> 再開始跑，確保程式一開始就受到限制 */
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        NULL, wdir, &si, &pi))
        goto cleanup;
    in_job = AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    start = GetTickCount64();
    result->status = RUN_OK;
    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 20);
        LARGE_INTEGER size;
        ULONGLONG elapsed = GetTickCount64() - start;

        if (limits->output_limit > 0 && stdout_path != NULL &&
            GetFileSizeEx(out, &size) && size.QuadPart > limits->output_limit) {
            result->status = RUN_OUTPUT_LIMIT;
            break;
        }
        if (wait == WAIT_OBJECT_0)
            break;
        if (limits->timeout_ms > 0 && elapsed > (ULONGLONG)limits->timeout_ms) {
            result->status = RUN_TIMEOUT;
            break;
        }
    }

    if (result->status != RUN_OK) {
        /* 放進 job 失敗 (極少見) 時，至少把程式本身結束，不能讓無窮迴圈一直跑 */
        if (in_job)
            TerminateJobObject(job, 1);
        else
            TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    result->elapsed_ms = (int)(GetTickCount64() - start);

    GetExitCodeProcess(pi.hProcess, &exit_code);
    result->exit_code = exit_code;
    if (result->status == RUN_OK && is_crash_exit_code(exit_code))
        result->status = RUN_RUNTIME_ERROR;

cleanup:
    if (pi.hProcess != NULL)
        CloseHandle(pi.hProcess);
    if (pi.hThread != NULL)
        CloseHandle(pi.hThread);
    if (job != NULL)
        CloseHandle(job); /* KILL_ON_JOB_CLOSE：殘留的子程式也一起結束 */
    if (in != INVALID_HANDLE_VALUE)
        CloseHandle(in);
    if (err != INVALID_HANDLE_VALUE && err != out)
        CloseHandle(err);
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    free(wcmd);
    free(wdir);
    return result->status;
}
