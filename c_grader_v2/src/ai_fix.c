/*
 * ai_fix.c - 呼叫本機 AI 命令列工具修正編譯錯誤 (說明見 ai_fix.h)。
 *
 * 呼叫方式與 tools\ai_*.ps1 相同：透過 cmd.exe 重新導向，提示詞從 stdin 進、回答從 stdout 出，
 * 位元組原封不動 (UTF-8)；但不開視窗，並用 executor 的 Job Object 控制時間限制，AI 卡住也會被結束。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_fix.h"
#include "executor.h"
#include "util.h"

#define AI_ANSWER_LIMIT (4LL * 1024 * 1024)
#define AI_LOG_IN_PROMPT_LIMIT (16 * 1024) /* 錯誤訊息太長時只送前面這麼多 */

typedef struct {
    const char *name;
    const char *command; /* 非互動模式：提示詞從 stdin 送進去，結果從 stdout 拿回來 */
} KnownTool;

/* 與 tools\ai_testcases.ps1 / ai_roster.ps1 的 $Commands 相同；工具版本參數不同時兩邊一起改 */
static const KnownTool known_tools[] = {
    {"claude", "claude -p --output-format text"},
    {"codex", "codex exec --skip-git-repo-check -"},
    {"gemini", "gemini -p \"請完成標準輸入中的任務\""},
};

#define KNOWN_TOOL_COUNT ((int)(sizeof(known_tools) / sizeof(known_tools[0])))

/* PATH 上找得到 name 嗎？npm 安裝的工具是 .cmd / .ps1，原生安裝的是 .exe */
static int on_path(const char *name)
{
    static const wchar_t *exts[] = {L".exe", L".cmd", L".bat", L".ps1"};
    wchar_t *w = utf8_to_wide(name), found[GRADER_PATH_MAX];
    size_t i;
    int ok = 0;

    if (w == NULL)
        return 0;
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]) && !ok; i++)
        ok = SearchPathW(NULL, w, exts[i], GRADER_PATH_MAX, found, NULL) > 0;
    free(w);
    return ok;
}

int ai_tool_find(const char *tool, char *name, size_t name_size, char *command, size_t command_size)
{
    int i;

    if (tool == NULL || tool[0] == '\0' || _stricmp(tool, "auto") == 0) {
        for (i = 0; i < KNOWN_TOOL_COUNT; i++) {
            if (on_path(known_tools[i].name)) {
                snprintf(name, name_size, "%s", known_tools[i].name);
                snprintf(command, command_size, "%s", known_tools[i].command);
                return 1;
            }
        }
        return 0;
    }
    for (i = 0; i < KNOWN_TOOL_COUNT; i++) {
        if (_stricmp(tool, known_tools[i].name) == 0) {
            if (!on_path(known_tools[i].name))
                return 0;
            snprintf(name, name_size, "%s", known_tools[i].name);
            snprintf(command, command_size, "%s", known_tools[i].command);
            return 1;
        }
    }
    /* 自訂命令列：顯示名稱取第一個字 */
    snprintf(command, command_size, "%s", tool);
    snprintf(name, name_size, "%.*s", (int)strcspn(tool, " \t"), tool);
    return 1;
}

static int is_blank(const char *s)
{
    for (; *s != '\0'; s++)
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
            return 0;
    return 1;
}

char *ai_ask(const char *command, const char *prompt, const char *work_dir, int timeout_ms, char *err, size_t err_size)
{
    char prompt_path[GRADER_PATH_MAX], answer_path[GRADER_PATH_MAX], err_path[GRADER_PATH_MAX];
    char comspec[GRADER_PATH_MAX] = "cmd.exe";
    wchar_t wcomspec[GRADER_PATH_MAX];
    StrBuf cmd = {0};
    RunLimits limits;
    RunResult run;
    char *answer, *detail;
    size_t len = 0;

    path_join(prompt_path, sizeof(prompt_path), work_dir, "ai_prompt.txt");
    path_join(answer_path, sizeof(answer_path), work_dir, "ai_answer.txt");
    path_join(err_path, sizeof(err_path), work_dir, "ai_error.txt");
    if (!write_file(prompt_path, prompt, strlen(prompt))) {
        snprintf(err, err_size, "無法寫入提示詞檔 %s", prompt_path);
        return NULL;
    }
    delete_file(answer_path);
    delete_file(err_path);

    /* 在 Claude Code 的終端機裡巢狀執行 claude 會被擋，先拿掉這個環境變數 (子程式會繼承) */
    SetEnvironmentVariableW(L"CLAUDECODE", NULL);

    if (GetEnvironmentVariableW(L"COMSPEC", wcomspec, GRADER_PATH_MAX) > 0) {
        char *s = wide_to_utf8(wcomspec);
        if (s != NULL) {
            snprintf(comspec, sizeof(comspec), "%s", s);
            free(s);
        }
    }
    /* cmd /s：去掉最外層引號後照字面執行；stderr 另外存檔，stdin/stdout 由 executor 接上 */
    sb_appendf(&cmd, "\"%s\" /d /s /c \"%s 2> \"%s\"\"", comspec, command, err_path);

    limits.timeout_ms = timeout_ms > 0 ? timeout_ms : 180000;
    limits.output_limit = AI_ANSWER_LIMIT;
    limits.memory_limit = 0;
    run_program(cmd.data, work_dir, prompt_path, answer_path, 0, &limits, &run);
    sb_free(&cmd);

    answer = read_file(answer_path, &len);
    if (run.status == RUN_OK && run.exit_code == 0 && answer != NULL && !is_blank(answer))
        return answer;

    /* 失敗：把原因與 stderr (沒有就用 stdout) 的前幾行寫進 err */
    detail = read_file_limited(err_path, 400, NULL, NULL);
    if ((detail == NULL || is_blank(detail)) && answer != NULL && !is_blank(answer)) {
        free(detail);
        detail = malloc(401);
        if (detail != NULL)
            snprintf(detail, 401, "%.400s", answer);
    }
    if (detail != NULL) { /* 只留第一個非空白行，避免訊息太長 */
        char *line = detail, *nl;
        while (*line == '\r' || *line == '\n' || *line == ' ')
            line++;
        nl = strpbrk(line, "\r\n");
        if (nl != NULL)
            *nl = '\0';
        memmove(detail, line, strlen(line) + 1);
    }
    if (run.status == RUN_TIMEOUT)
        snprintf(err, err_size, "AI 工具超過 %d 秒沒有回應", limits.timeout_ms / 1000);
    else if (run.status == RUN_START_FAILED)
        snprintf(err, err_size, "無法啟動 %s", comspec);
    else if (run.status != RUN_OK)
        snprintf(err, err_size, "AI 工具異常結束 (%s)", run_status_name(run.status));
    else if (run.exit_code != 0)
        snprintf(err, err_size, "AI 工具結束碼 %lu", run.exit_code);
    else
        snprintf(err, err_size, "AI 沒有回傳任何內容");
    if (detail != NULL && !is_blank(detail)) {
        size_t n = strlen(err);
        if (n < err_size)
            snprintf(err + n, err_size - n, "：%s", detail);
    }
    free(detail);
    free(answer);
    return NULL;
}

/* 這一行是不是 Markdown 圍欄 (``` 或 ```c)？回傳行首指標，不是回傳 NULL */
static const char *fence_line(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    return strncmp(p, "```", 3) == 0 ? line : NULL;
}

static const char *next_line(const char *p)
{
    const char *nl = strchr(p, '\n');
    return nl != NULL ? nl + 1 : p + strlen(p);
}

char *ai_extract_code(const char *answer)
{
    const char *p = answer, *start, *end, *line;
    StrBuf out = {0};
    int in_fence = 0;

    if (answer == NULL)
        return NULL;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    /* 有圍欄就只取第一個圍欄裡的內容 (AI 常在前後加說明)，沒有就整段 */
    start = p;
    end = p + strlen(p);
    for (line = p; *line != '\0'; line = next_line(line)) {
        if (fence_line(line) == NULL)
            continue;
        if (!in_fence) {
            in_fence = 1;
            start = next_line(line);
        } else {
            end = line;
            break;
        }
    }
    if (in_fence && end < start) /* 只有開頭圍欄沒有結尾：取到最後 */
        end = p + strlen(p);

    for (; start < end; start++)
        if (*start != '\r') {
            char one[2] = {*start, '\0'};
            sb_append(&out, one);
        }
    while (out.len > 0 && (out.data[out.len - 1] == '\n' || out.data[out.len - 1] == ' ' ||
                           out.data[out.len - 1] == '\t'))
        out.data[--out.len] = '\0';
    if (out.len == 0) {
        sb_free(&out);
        return NULL;
    }
    sb_append(&out, "\n");
    return out.data;
}

char *ai_fix_prompt(const char *file_name, const char *source, const char *compile_log, int attempt)
{
    StrBuf sb = {0};
    size_t log_len = compile_log != NULL ? strlen(compile_log) : 0;

    sb_append(&sb,
              "你是 C 語言課程的助教。下面這份學生作業程式用 gcc 編譯失敗。\n"
              "請做「最小修改」讓它能夠編譯成功，規則：\n"
              "1. 只修正造成編譯錯誤的地方：例如少打分號、拼錯的識別字或函式名稱、缺少 #include、"
              "括號或引號不對稱、型別或宣告寫錯。\n"
              "2. 絕對不可以修正邏輯錯誤、不可以改變任何 printf / scanf 的文字與格式、"
              "不可以增刪與編譯錯誤無關的程式碼。\n"
              "3. 不可以重新排版：保留原本的縮排、空白、空行、註解與換行方式，沒有錯誤的行一個字都不要動。\n"
              "4. 改動的字元數越少越好——學生會依照「被修改的字元數」扣分，多改一個字就是多扣一分。\n"
              "5. 只輸出修正後的完整程式碼 (從第一行到最後一行)，不要任何說明文字，不要 Markdown 程式碼區塊。\n\n");
    if (attempt >= 2)
        sb_appendf(&sb, "注意：這是第 %d 次嘗試。你上一次修正後的程式碼 (見下方) 仍然無法編譯，"
                        "請針對新的 gcc 錯誤訊息再做最小修正。\n\n", attempt);

    sb_append(&sb, "gcc 錯誤訊息：\n");
    if (log_len > AI_LOG_IN_PROMPT_LIMIT) {
        sb_appendf(&sb, "%.*s\n…(以下省略)\n", AI_LOG_IN_PROMPT_LIMIT, compile_log);
    } else {
        sb_append(&sb, log_len > 0 ? compile_log : "(gcc 沒有輸出任何訊息)");
        if (log_len == 0 || compile_log[log_len - 1] != '\n')
            sb_append(&sb, "\n");
    }
    sb_appendf(&sb, "\n%s (%s)：\n", attempt >= 2 ? "你上次修正後的程式碼" : "學生程式", file_name);
    sb_append(&sb, source);
    if (source[0] == '\0' || source[strlen(source) - 1] != '\n')
        sb_append(&sb, "\n");
    return sb.data;
}
