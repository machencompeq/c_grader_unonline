#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_fix.h"
#include "comparator.h"
#include "compiler.h"
#include "grader.h"
#include "scorer.h"

#define SOURCE_SEARCH_DEPTH 3
/* 時間限制很短時，超時可能只是電腦剛好忙；低於這個值才重跑確認 */
#define TLE_RECHECK_BELOW_MS 10000

const char *test_status_name(TestStatus status)
{
    switch (status) {
    case TEST_PASS:          return "AC";
    case TEST_WRONG:         return "WA";
    case TEST_RUNTIME_ERROR: return "RE";
    case TEST_TIMEOUT:       return "TLE";
    case TEST_OUTPUT_LIMIT:  return "OLE";
    case TEST_NOT_RUN:       return "-";
    }
    return "?";
}

const char *test_status_text(TestStatus status)
{
    switch (status) {
    case TEST_PASS:          return "完全正確";
    case TEST_WRONG:         return "輸出有差異";
    case TEST_RUNTIME_ERROR: return "Runtime Error (程式當掉)";
    case TEST_TIMEOUT:       return "Time Limit Exceeded (超過時間)";
    case TEST_OUTPUT_LIMIT:  return "Output Limit Exceeded (輸出過大)";
    case TEST_NOT_RUN:       return "未執行";
    }
    return "?";
}

const char *student_status_name(StudentStatus status)
{
    switch (status) {
    case STUDENT_OK:            return "OK";
    case STUDENT_CE_FIXED:      return "CE Fixed";
    case STUDENT_COMPILE_ERROR: return "Compile Error";
    case STUDENT_NO_SOURCE:     return "No Source";
    }
    return "?";
}

const char *student_status_text(StudentStatus status)
{
    switch (status) {
    case STUDENT_OK:            return "OK";
    case STUDENT_CE_FIXED:      return "CE→AI 修正";
    case STUDENT_COMPILE_ERROR: return "編譯失敗";
    case STUDENT_NO_SOURCE:     return "沒有 .c 檔";
    }
    return "?";
}

int student_ran_tests(StudentStatus status)
{
    return status == STUDENT_OK || status == STUDENT_CE_FIXED;
}

static void student_limits(const Grader *g, RunLimits *limits)
{
    limits->timeout_ms = g->cfg.timeout_ms;
    limits->output_limit = g->cfg.output_limit;
    limits->memory_limit = g->cfg.memory_limit;
}

static void append_note(char *note, size_t size, const char *text)
{
    size_t len = strlen(note);
    snprintf(note + len, size - len, "%s%s", len > 0 ? "；" : "", text);
}

/* 這些資料夾不是學生 */
static int is_ignored_folder(const char *name)
{
    return name[0] == '.' || _stricmp(name, "reference") == 0 || _stricmp(name, "testcase") == 0 ||
           _stricmp(name, "temp") == 0 || _stricmp(name, "reports") == 0 || _stricmp(name, "__MACOSX") == 0;
}

/* 找出資料夾裡所有 .c 檔 (完整路徑) */
static void find_sources(const char *dir, StringList *sources)
{
    StringList names = {0};
    int i;

    list_dir(dir, "*.c", 0, &names);
    for (i = 0; i < names.count; i++) {
        char full[GRADER_PATH_MAX];
        size_t len = strlen(names.items[i]);
        if (len < 2 || _stricmp(names.items[i] + len - 2, ".c") != 0)
            continue;
        path_join(full, sizeof(full), dir, names.items[i]);
        string_list_add(sources, full);
    }
    string_list_free(&names);
}

/*
 * 學生資料夾第一層沒有 .c 時，往子資料夾找 (例如學生把整個專案資料夾交上來)。
 * 找到第一個有 .c 的資料夾就停，relative 記錄相對路徑。
 */
static int find_sources_nested(const char *dir, int depth, StringList *sources, char *relative, size_t size)
{
    StringList subdirs = {0};
    int i, found = 0;

    find_sources(dir, sources);
    if (sources->count > 0)
        return 1;
    if (depth >= SOURCE_SEARCH_DEPTH)
        return 0;

    list_dir(dir, "*", 1, &subdirs);
    for (i = 0; i < subdirs.count && !found; i++) {
        char child[GRADER_PATH_MAX];
        size_t len = strlen(relative);
        if (is_ignored_folder(subdirs.items[i]))
            continue;
        path_join(child, sizeof(child), dir, subdirs.items[i]);
        snprintf(relative + len, size - len, "%s%s", len > 0 ? "\\" : "", subdirs.items[i]);
        found = find_sources_nested(child, depth + 1, sources, relative, size);
        if (!found)
            relative[len] = '\0';
    }
    string_list_free(&subdirs);
    return found;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '\\');
    return slash != NULL ? slash + 1 : path;
}

/*
 * 公平性：stdout 導向檔案時，C 會先把 printf 的內容暫存在記憶體，最後才寫出；
 * 程式如果在最後才當掉 (例如陣列太小、中文輸入溢位，return 時才出錯)，暫存內容會整個遺失，
 * 變成「什麼都沒印」。但學生在終端機 (Dev-C++、onlinegdb) 看到的是完整輸出。
 * 所以每支程式都一起編譯這個小檔案：main 開始前把 stdout 設成不暫存，印出的內容一定留得住。
 */
static const char *STDOUT_HELPER_SOURCE =
    "/* 批改工具自動加入：讓 stdout 不暫存，程式當掉前印出的內容也能完整保留 */\n"
    "#include <stdio.h>\n"
    "__attribute__((constructor)) static void c_grader_unbuffered_stdout(void)\n"
    "{\n"
    "    setvbuf(stdout, NULL, _IONBF, 0);\n"
    "}\n";

static int compile_with_helper(const Grader *g, const char *flags, const StringList *sources, const char *exe,
                               const char *work_dir, CompileResult *cr)
{
    StringList all = {0};
    int i, ok;

    for (i = 0; i < sources->count; i++)
        string_list_add(&all, sources->items[i]);
    if (g->stdout_helper[0] != '\0')
        string_list_add(&all, g->stdout_helper);
    ok = compile_sources(g->cfg.gcc_path, flags, &all, exe, work_dir, cr);
    string_list_free(&all);
    return ok;
}

/*
 * 要交給 gcc 的檔案：UTF-8 的原始碼直接用；不是 UTF-8 (Big5) 的就在 work_dir 放一份轉成 UTF-8 的副本。
 * 自己轉換、不依賴 gcc 的 -finput-charset (有些 MinGW 沒有編進 iconv)。回傳轉換了幾個檔案。
 */
static int prepare_sources(const StringList *sources, const char *work_dir, StringList *prepared)
{
    int i, converted = 0;

    for (i = 0; i < sources->count; i++) {
        size_t len = 0;
        char *text = read_file(sources->items[i], &len);
        char copy[GRADER_PATH_MAX], name[300];

        if (text == NULL || is_valid_utf8(text, len)) {
            free(text);
            string_list_add(prepared, sources->items[i]);
            continue;
        }
        text = text_to_utf8(text, &len);
        snprintf(name, sizeof(name), "utf8_%d_%s", i, base_name(sources->items[i]));
        path_join(copy, sizeof(copy), work_dir, name);
        if (write_file(copy, text, len)) {
            string_list_add(prepared, copy);
            converted++;
        } else {
            string_list_add(prepared, sources->items[i]);
        }
        free(text);
    }
    return converted;
}

/*
 * 編譯，並自動處理兩種常見、但不是學生程式邏輯錯誤的情況 (公平性)：
 *   1. 原始碼存成 Big5 (舊版 Dev-C++ / Visual Studio 預設) -> 自動轉成 UTF-8 再編譯
 *   2. 資料夾裡有多個 .c (例如 main.c 與 main_old.c 都有 main) -> 改成只編譯其中一個
 * 所做的處理都會寫進 note，老師在報告裡看得到。sources 保持原始路徑 (報告要顯示學生的檔案)。
 */
static int compile_with_fallbacks(const Grader *g, StringList *sources, const char *exe, const char *work_dir,
                                  CompileResult *cr, char *note, size_t note_size)
{
    StringList prepared = {0};
    int i, pass, converted, ok = 0;

    converted = prepare_sources(sources, work_dir, &prepared);
    compile_with_helper(g, g->cfg.compile_flags, &prepared, exe, work_dir, cr);
    if (cr->ok) {
        if (converted > 0)
            append_note(note, note_size, "原始碼不是 UTF-8，已視為 Big5 自動轉換後編譯");
    } else if (converted > 0) {
        /* 轉換後反而編不過 (極少見)：改用原始 bytes 編譯 */
        CompileResult raw;
        compile_with_helper(g, g->cfg.compile_flags, sources, exe, work_dir, &raw);
        if (raw.ok) {
            append_note(note, note_size, "原始碼不是 UTF-8 且轉換後無法編譯，已直接編譯；程式印出的中文可能是亂碼");
            compile_result_free(cr);
            *cr = raw;
        } else {
            compile_result_free(&raw);
        }
    }
    if (cr->ok || sources->count <= 1) {
        string_list_free(&prepared);
        return cr->ok;
    }

    /* 多個 .c 一起編譯失敗：先試 main.c，再依序試其他檔案 */
    for (pass = 0; pass < 2 && !ok; pass++) {
        for (i = 0; i < sources->count && !ok; i++) {
            int is_main = _stricmp(base_name(sources->items[i]), "main.c") == 0;
            StringList one = {0};
            CompileResult single;
            char msg[512];

            if ((pass == 0) != is_main)
                continue;
            string_list_add(&one, prepared.items[i]);
            compile_with_helper(g, g->cfg.compile_flags, &one, exe, work_dir, &single);
            string_list_free(&one);
            if (single.ok) {
                snprintf(msg, sizeof(msg), "資料夾有 %d 個 .c 一起編譯失敗，改為只編譯 %s", sources->count,
                         base_name(sources->items[i]));
                append_note(note, note_size, msg);
                compile_result_free(cr);
                *cr = single;
                string_list_add(&one, sources->items[i]);
                string_list_free(sources);
                *sources = one;
                ok = 1;
            } else {
                compile_result_free(&single);
            }
        }
    }
    string_list_free(&prepared);
    return ok;
}

/* ---------------- 編譯失敗的 AI 最小修正 ---------------- */

/* 修正結果快取：測資資料夾\ai_fix_cache\<原始碼雜湊>.c (同一份原始碼重新批改時結果不變，也不必再問 AI) */
static void fix_cache_path(const Grader *g, const char *source, size_t len, char *out, size_t size)
{
    unsigned long long h = 1469598103934665603ULL; /* FNV-1a 64 */
    char dir[GRADER_PATH_MAX], name[40];
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (unsigned char)source[i];
        h *= 1099511628211ULL;
    }
    path_join(dir, sizeof(dir), g->problem_dir, "ai_fix_cache");
    snprintf(name, sizeof(name), "%016llx.c", h);
    path_join(out, size, dir, name);
}

static void fix_cache_store(const Grader *g, const char *path, const char *code)
{
    char dir[GRADER_PATH_MAX];
    path_join(dir, sizeof(dir), g->problem_dir, "ai_fix_cache");
    make_dir(dir);
    write_file(path, code, strlen(code));
}

/*
 * 編譯失敗時的 AI 最小修正 (設定 ai_fix)：
 *   (快取有就直接用) 原始碼 + gcc 錯誤訊息 -> 本機 AI -> 修正後原始碼 -> 再編譯；
 *   仍失敗就把新錯誤再交給 AI (最多 attempts 次)。
 * 成功回傳 1：exe 已編好，r->fixed_source / fix_chars / fix_tool 填好，之後照常執行測資。
 * 失敗回傳 0 (維持 Compile Error，給保底分)，原因寫進 r->note；
 * 最後一次的修正碼與錯誤留在 r->fixed_source / fix_log 供報告顯示。
 * AI 修好但改超過 ai_fix_max_chars 個字：不採用 (可能順手修了邏輯)，fix_rejected = 1。
 */
static int try_ai_fix(Grader *g, StudentResult *r, const char *work_dir, const char *exe, const char *ce_log)
{
    char *original, *current_source = NULL, *current_log = NULL, msg[600], cache_path[GRADER_PATH_MAX];
    size_t original_len = 0;
    const char *file_name;
    int attempt, ok = 0, ai_failed = 0;
    CompareOptions opts;

    if (r->sources.count != 1) {
        snprintf(msg, sizeof(msg), "有 %d 個 .c 檔，AI 自動修正只支援單一檔案", r->sources.count);
        append_note(r->note, sizeof(r->note), msg);
        return 0;
    }
    original = read_file(r->sources.items[0], &original_len);
    if (original == NULL) {
        append_note(r->note, sizeof(r->note), "無法讀取原始碼，無法自動修正");
        return 0;
    }
    original = text_to_utf8(original, &original_len); /* Big5 先轉 UTF-8，修正距離以字元計 */
    file_name = base_name(r->sources.items[0]);
    fix_cache_path(g, original, original_len, cache_path, sizeof(cache_path));

    /* 1. 快取：上次修好的程式碼 */
    if (g->cfg.ai_fix_cache && file_exists(cache_path)) {
        char *cached = read_file(cache_path, NULL);
        if (cached != NULL) {
            StringList one = {0};
            CompileResult fix_cr;
            string_list_add(&one, cache_path);
            compile_with_helper(g, g->cfg.compile_flags, &one, exe, work_dir, &fix_cr);
            string_list_free(&one);
            if (fix_cr.ok) {
                current_source = cached;
                current_log = fix_cr.log != NULL ? fix_cr.log : _strdup("");
                ok = 1;
                r->fix_cached = 1;
                r->fix_attempts = 0;
                snprintf(r->fix_tool, sizeof(r->fix_tool), "快取");
            } else {
                compile_result_free(&fix_cr);
                free(cached);
            }
        }
    }

    /* 2. 問 AI */
    if (!ok) {
        if (!g->ai_checked) {
            g->ai_checked = 1;
            g->ai_available = ai_tool_find(g->cfg.ai_fix_tool, g->ai_tool, sizeof(g->ai_tool), g->ai_command,
                                           sizeof(g->ai_command));
        }
        if (!g->ai_available) {
            append_note(r->note, sizeof(r->note), "找不到本機 AI 工具 (claude / codex / gemini)，無法自動修正編譯錯誤");
            free(original);
            return 0;
        }
        snprintf(r->fix_tool, sizeof(r->fix_tool), "%s", g->ai_tool);
        current_source = _strdup(original);
        current_log = _strdup(ce_log != NULL ? ce_log : "");
        for (attempt = 1; attempt <= g->cfg.ai_fix_attempts && !ok; attempt++) {
            char *prompt = ai_fix_prompt(file_name, current_source, current_log, attempt);
            char err[512], fixed_path[GRADER_PATH_MAX], fixed_name[64], *answer, *code;
            StringList one = {0};
            CompileResult fix_cr;

            r->fix_attempts = attempt;
            answer = ai_ask(g->ai_command, prompt, work_dir, g->cfg.ai_fix_timeout_ms, err, sizeof(err));
            free(prompt);
            if (answer == NULL) {
                snprintf(msg, sizeof(msg), "AI (%s) 修正失敗：%s", g->ai_tool, err);
                append_note(r->note, sizeof(r->note), msg);
                ai_failed = 1;
                break;
            }
            code = ai_extract_code(answer);
            free(answer);
            if (code == NULL) {
                free(current_log);
                current_log = _strdup("(AI 沒有回傳任何程式碼，請只輸出完整的 C 程式碼)");
                continue;
            }

            snprintf(fixed_name, sizeof(fixed_name), "ai_fix_%d.c", attempt);
            path_join(fixed_path, sizeof(fixed_path), work_dir, fixed_name);
            write_file(fixed_path, code, strlen(code));
            string_list_add(&one, fixed_path);
            compile_with_helper(g, g->cfg.compile_flags, &one, exe, work_dir, &fix_cr);
            string_list_free(&one);

            free(current_source);
            current_source = code;
            free(current_log);
            current_log = fix_cr.log != NULL ? fix_cr.log : _strdup("");
            ok = fix_cr.ok;
        }
        if (ok && g->cfg.ai_fix_cache)
            fix_cache_store(g, cache_path, current_source);
    }

    /* 3. 修正字元數；改太多就不採用 */
    if (ok) {
        compare_options_default(&opts);
        opts.ignore_trailing_space = 1; /* CRLF/LF、行尾空白、檔尾換行不算修改 */
        r->fix_chars = compare_outputs(original, original_len, current_source, strlen(current_source), &opts,
                                       &r->fix_approximate);
        if (g->cfg.ai_fix_max_chars > 0 && r->fix_chars > g->cfg.ai_fix_max_chars) {
            snprintf(msg, sizeof(msg), "AI 修改了 %ld 個字，超過上限 %d 個，可能動到邏輯，不採用；給編譯失敗保底分，請老師確認",
                     r->fix_chars, g->cfg.ai_fix_max_chars);
            append_note(r->note, sizeof(r->note), msg);
            r->fix_rejected = 1;
            ok = 0;
        } else if (r->fix_cached) {
            snprintf(msg, sizeof(msg), "編譯失敗，沿用上次 AI 的最小修正 (%ld 個字元) 後繼續批改", r->fix_chars);
            append_note(r->note, sizeof(r->note), msg);
        } else if (r->fix_attempts > 1) {
            snprintf(msg, sizeof(msg), "編譯失敗，AI (%s) 第 %d 次嘗試最小修正 %ld 個字元後可編譯，已繼續批改",
                     g->ai_tool, r->fix_attempts, r->fix_chars);
            append_note(r->note, sizeof(r->note), msg);
        } else {
            snprintf(msg, sizeof(msg), "編譯失敗，AI (%s) 最小修正 %ld 個字元後可編譯，已繼續批改", g->ai_tool,
                     r->fix_chars);
            append_note(r->note, sizeof(r->note), msg);
        }
    } else if (!ai_failed) {
        snprintf(msg, sizeof(msg), "AI (%s) 嘗試修正 %d 次後仍無法編譯，維持 Compile Error", g->ai_tool,
                 r->fix_attempts);
        append_note(r->note, sizeof(r->note), msg);
    }
    /* 成功或失敗都保留最後一次的程式碼與訊息：老師可以在報告裡看到 AI 改了什麼 */
    if (current_source != NULL && strcmp(current_source, original) != 0) {
        r->fixed_source = current_source;
        r->fix_log = current_log;
    } else {
        free(current_source);
        free(current_log);
    }
    free(original);
    return ok;
}

void grader_resolve_students_dir(const char *folder, char *out, size_t size)
{
    char sub[GRADER_PATH_MAX];
    path_join(sub, sizeof(sub), folder, "students");
    snprintf(out, size, "%s", dir_exists(sub) ? sub : folder);
}

void grader_problem_paths(const char *problem, char *reference, char *data_dir, size_t size)
{
    char full[GRADER_PATH_MAX];

    if (!absolute_path(problem, full, sizeof(full)))
        snprintf(full, sizeof(full), "%s", problem);

    if (file_exists(full)) {
        /* 題目 = 一個 .c 檔：D:\hw\hw1.c -> 測資放在 D:\hw\hw1_測資 */
        char *dot, *slash;
        snprintf(reference, size, "%s", full);
        dot = strrchr(full, '.');
        slash = strrchr(full, '\\');
        if (dot != NULL && (slash == NULL || dot > slash))
            *dot = '\0';
        snprintf(data_dir, size, "%s_測資", full);
    } else {
        /* 舊格式：題目資料夾\reference\*.c */
        path_join(reference, size, full, "reference");
        snprintf(data_dir, size, "%s", full);
    }
}

static void reference_sources(const Grader *g, StringList *out)
{
    if (file_exists(g->reference))
        string_list_add(out, g->reference);
    else
        find_sources(g->reference, out);
}

int grader_open(Grader *g, const char *problem, const char *students_dir, const GradeConfig *cfg,
                char *err, size_t err_size)
{
    return grader_open_ex(g, problem, NULL, students_dir, cfg, err, err_size);
}

int grader_open_ex(Grader *g, const char *problem, const char *data_dir, const char *students_dir,
                   const GradeConfig *cfg, char *err, size_t err_size)
{
    char path[GRADER_PATH_MAX], resolved[GRADER_PATH_MAX];
    StringList listed = {0}, ref = {0};
    int i;

    memset(g, 0, sizeof(*g));
    g->cfg = *cfg;
    grader_problem_paths(problem, g->reference, g->problem_dir, sizeof(g->problem_dir));
    if (data_dir != NULL)
        snprintf(g->problem_dir, sizeof(g->problem_dir), "%s", data_dir);

    reference_sources(g, &ref);
    i = ref.count;
    string_list_free(&ref);
    if (i == 0) {
        snprintf(err, err_size, "找不到參考答案：%s", g->reference);
        return 0;
    }

    if (students_dir != NULL && students_dir[0] != '\0') {
        grader_resolve_students_dir(students_dir, resolved, sizeof(resolved));
    } else if (!file_exists(g->reference)) {
        grader_resolve_students_dir(g->problem_dir, resolved, sizeof(resolved)); /* 舊格式：題目資料夾\students */
    } else {
        resolved[0] = '\0'; /* 只驗證參考答案，不需要學生 */
    }
    if (resolved[0] != '\0' &&
        (!absolute_path(resolved, g->students_dir, sizeof(g->students_dir)) || !dir_exists(g->students_dir))) {
        snprintf(err, err_size, "找不到學生資料夾：%s", resolved);
        return 0;
    }

    if (!testcase_scan(g->problem_dir, &g->tests, err, err_size))
        return 0;

    /* 老師若把參考答案 .c (或它的測資資料夾) 放在學生資料夾裡，不能把它們當成學生 */
    if (g->students_dir[0] != '\0')
        grader_list_students(g->students_dir, &listed);
    for (i = 0; i < listed.count; i++) {
        char full[GRADER_PATH_MAX];
        path_join(full, sizeof(full), g->students_dir, listed.items[i]);
        if (_stricmp(full, g->reference) != 0 && _stricmp(full, g->problem_dir) != 0)
            string_list_add(&g->students, listed.items[i]);
    }
    string_list_free(&listed);

    roster_default_path(path, sizeof(path));
    roster_load(path, &g->roster);

    /* 暫存放在系統暫存資料夾，不會弄髒學生或題目資料夾 */
    if (!make_run_temp_dir(g->temp_dir, sizeof(g->temp_dir))) {
        snprintf(err, err_size, "無法建立暫存資料夾");
        testcase_free(&g->tests);
        string_list_free(&g->students);
        roster_free(&g->roster);
        return 0;
    }
    path_join(g->stdout_helper, sizeof(g->stdout_helper), g->temp_dir, "c_grader_stdout.c");
    if (!write_file(g->stdout_helper, STDOUT_HELPER_SOURCE, strlen(STDOUT_HELPER_SOURCE)))
        g->stdout_helper[0] = '\0'; /* 寫不出來就照原本方式編譯 */

    g->expected = calloc(g->tests.count, sizeof(char *));
    g->expected_len = calloc(g->tests.count, sizeof(size_t));
    if (g->expected == NULL || g->expected_len == NULL) {
        snprintf(err, err_size, "記憶體不足");
        grader_close(g);
        return 0;
    }
    return 1;
}

int grader_prepare_reference(Grader *g, ReferenceReport *report, char *err, size_t err_size)
{
    char work_dir[GRADER_PATH_MAX], exe[GRADER_PATH_MAX], cmd[GRADER_PATH_MAX + 4];
    char note[512] = "";
    StringList sources = {0};
    CompileResult cr;
    RunLimits limits;
    int i;

    memset(report, 0, sizeof(*report));
    report->count = g->tests.count;
    report->checks = calloc(g->tests.count > 0 ? g->tests.count : 1, sizeof(ReferenceCheck));
    if (report->checks == NULL) {
        snprintf(err, err_size, "記憶體不足");
        return 0;
    }

    path_join(work_dir, sizeof(work_dir), g->temp_dir, "_reference");
    make_dir(work_dir);
    path_join(exe, sizeof(exe), work_dir, "reference.exe");

    reference_sources(g, &sources);
    compile_with_fallbacks(g, &sources, exe, work_dir, &cr, note, sizeof(note));
    string_list_free(&sources);
    report->compiled = cr.ok;
    report->compile_log = cr.log;
    if (!cr.ok) {
        snprintf(err, err_size, "參考答案編譯失敗，停止批改。");
        return 0;
    }

    student_limits(g, &limits);
    snprintf(cmd, sizeof(cmd), "\"%s\"", exe);

    for (i = 0; i < g->tests.count; i++) {
        const TestCase *tc = &g->tests.items[i];
        ReferenceCheck *check = &report->checks[i];
        char out_path[GRADER_PATH_MAX], out_name[300];
        RunResult run;
        char *output;
        size_t output_len = 0;

        snprintf(check->name, sizeof(check->name), "%s", tc->name);
        check->has_out = tc->has_out;
        check->diff = -1;

        snprintf(out_name, sizeof(out_name), "%s_reference_output.txt", tc->name);
        path_join(out_path, sizeof(out_path), work_dir, out_name);
        run_program(cmd, work_dir, tc->in_path, out_path, 0, &limits, &run);
        check->run_status = run.status;
        output = read_file(out_path, &output_len);

        if (tc->has_out) {
            g->expected[i] = read_file(tc->out_path, &g->expected_len[i]);
            if (g->expected[i] == NULL) {
                snprintf(err, err_size, "無法讀取 %s", tc->out_path);
                free(output);
                return 0;
            }
            if (run.status != RUN_OK) {
                report->failures++;
            } else if (output != NULL) {
                int approx;
                check->diff = compare_outputs(g->expected[i], g->expected_len[i], output, output_len,
                                              &g->cfg.compare, &approx);
                if (check->diff != 0)
                    report->mismatches++;
            }
            free(output);
        } else {
            /* 沒有 .out：參考程式的輸出就是標準答案，所以它一定要正常執行 */
            if (run.status != RUN_OK || output == NULL) {
                report->failures++;
                snprintf(err, err_size, "%s 沒有 .out 檔，且參考程式執行失敗 (%s)，無法產生標準答案。",
                         tc->name, run_status_name(run.status));
                free(output);
                return 0;
            }
            g->expected[i] = output;
            g->expected_len[i] = output_len;
        }
    }

    g->reference_ready = 1;
    return 1;
}

static void keep_preview(TestResult *t, const char *output, size_t len)
{
    size_t keep = len > OUTPUT_PREVIEW_LIMIT ? OUTPUT_PREVIEW_LIMIT : len;

    t->actual = malloc(keep + 1);
    if (t->actual == NULL)
        return;
    memcpy(t->actual, output, keep);
    t->actual[keep] = '\0';
    t->actual_len = keep;
    t->actual_truncated = keep < len;
}

void grader_grade_student(Grader *g, int index, StudentResult *r)
{
    char student_dir[GRADER_PATH_MAX], work_dir[GRADER_PATH_MAX], exe[GRADER_PATH_MAX];
    char cmd[GRADER_PATH_MAX + 4], relative[GRADER_PATH_MAX] = "", work_name[32];
    CompileResult cr;
    RunLimits limits;
    double per_test_full;
    int i, retried_tle = 0, confirmed_tle = 0;

    memset(r, 0, sizeof(*r));
    snprintf(r->id, sizeof(r->id), "%s", g->students.items[index]);
    r->fix_chars = -1;
    {
        const RosterEntry *e = roster_find(&g->roster, r->id);
        if (e != NULL) {
            snprintf(r->name, sizeof(r->name), "%s", e->name);
            snprintf(r->sid, sizeof(r->sid), "%s", e->sid);
        }
        if (r->sid[0] == '\0')
            roster_guess_sid(r->id, r->sid, sizeof(r->sid));
    }
    r->tests = calloc(g->tests.count, sizeof(TestResult));
    if (r->tests == NULL) {
        r->status = STUDENT_NO_SOURCE;
        snprintf(r->note, sizeof(r->note), "記憶體不足，無法批改");
        return;
    }
    r->test_count = g->tests.count;
    per_test_full = g->cfg.full_score / g->tests.count; /* 每個 testcase 等權重 */

    for (i = 0; i < r->test_count; i++) {
        r->tests[i].status = TEST_NOT_RUN;
        r->tests[i].diff = -1;
        r->tests[i].full = per_test_full;
        r->tests[i].deduction = per_test_full;
    }
    r->failed = r->test_count;
    r->total_diff = -1;
    r->total_deduction = g->cfg.full_score;

    /* Step 1：找原始碼並編譯 */
    path_join(student_dir, sizeof(student_dir), g->students_dir, r->id);
    grader_find_student_sources(student_dir, &r->sources, relative, sizeof(relative));
    if (r->sources.count == 0) {
        r->status = STUDENT_NO_SOURCE;
        return;
    }
    if (relative[0] != '\0') {
        char msg[GRADER_PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "原始碼在子資料夾 %s 中", relative);
        append_note(r->note, sizeof(r->note), msg);
    }

    /* 暫存資料夾用編號命名，學號有特殊字元也不會出問題 */
    snprintf(work_name, sizeof(work_name), "s%04d", index);
    path_join(work_dir, sizeof(work_dir), g->temp_dir, work_name);
    make_dir(work_dir);
    path_join(exe, sizeof(exe), work_dir, "student.exe");

    compile_with_fallbacks(g, &r->sources, exe, work_dir, &cr, r->note, sizeof(r->note));
    r->compile_log = cr.log;
    if (!cr.ok) {
        /* 編譯失敗：設定允許時交給本機 AI 做最小修正，修得好就用修正後的程式繼續批改 */
        if (!(g->cfg.ai_fix && try_ai_fix(g, r, work_dir, exe, cr.log))) {
            /* 編譯失敗不會是 0 分：給保底分 */
            double floor_score = config_ce_floor(&g->cfg);
            r->status = STUDENT_COMPILE_ERROR;
            if (floor_score > 0) {
                char msg[128], num[32];
                format_score(num, sizeof(num), floor_score);
                snprintf(msg, sizeof(msg), "編譯失敗保底 %s 分", num);
                append_note(r->note, sizeof(r->note), msg);
                r->score = floor_score;
                r->total_deduction = g->cfg.full_score - floor_score;
                r->ce_floor_applied = 1;
            }
            return;
        }
        r->status = STUDENT_CE_FIXED;
    } else {
        r->status = STUDENT_OK;
    }

    /* Step 2：逐一執行 testcase、比對、評分 */
    student_limits(g, &limits);
    snprintf(cmd, sizeof(cmd), "\"%s\"", exe);
    r->total_diff = 0;
    r->total_deduction = 0;
    r->failed = 0;

    for (i = 0; i < r->test_count; i++) {
        const TestCase *tc = &g->tests.items[i];
        TestResult *t = &r->tests[i];
        char out_path[GRADER_PATH_MAX], out_name[32];
        RunResult run;
        char *output;
        size_t output_len = 0;
        int score_output, truncated = 0;

        snprintf(out_name, sizeof(out_name), "out%03d.txt", i);
        path_join(out_path, sizeof(out_path), work_dir, out_name);
        run_program(cmd, work_dir, tc->in_path, out_path, 0, &limits, &run);

        /* 公平性：超時可能是電腦剛好忙 (防毒掃描新 exe 等)，重跑一次確認 */
        /* (已經確認過一次真的超時，就不再重跑，避免無窮迴圈的程式拖太久) */
        if (run.status == RUN_TIMEOUT && !confirmed_tle && g->cfg.timeout_ms < TLE_RECHECK_BELOW_MS) {
            RunResult again;
            run_program(cmd, work_dir, tc->in_path, out_path, 0, &limits, &again);
            if (again.status == RUN_TIMEOUT) {
                confirmed_tle = 1;
            } else if (!retried_tle) {
                append_note(r->note, sizeof(r->note), "第一次執行超時，重跑後正常");
                retried_tle = 1;
            }
            run = again;
        }
        t->elapsed_ms = run.elapsed_ms;
        t->exit_code = run.exit_code;

        /* TLE / OLE 不計分，輸出檔可能非常大：只讀前面一段給畫面看 */
        if (run.status == RUN_OK || run.status == RUN_RUNTIME_ERROR)
            output = read_file(out_path, &output_len);
        else
            output = read_file_limited(out_path, OUTPUT_PREVIEW_LIMIT, &output_len, &truncated);
        if (output != NULL) {
            keep_preview(t, output, output_len);
            if (truncated)
                t->actual_truncated = 1;
        }

        /* TLE / OLE：0 分。RE：預設仍比對已經印出的內容 (可設定成直接 0 分) */
        score_output = output != NULL &&
                       (run.status == RUN_OK ||
                        (run.status == RUN_RUNTIME_ERROR && !g->cfg.runtime_error_zero));

        if (score_output) {
            t->diff = compare_outputs(g->expected[i], g->expected_len[i], output, output_len,
                                      &g->cfg.compare, &t->approximate);
            t->score = score_apply(per_test_full, score_penalty(&g->cfg.rules, t->diff));
            r->total_diff += t->diff;
        } else {
            t->diff = -1;
            t->score = 0;
        }
        t->deduction = per_test_full - t->score;

        if (run.status == RUN_RUNTIME_ERROR || run.status == RUN_START_FAILED)
            t->status = TEST_RUNTIME_ERROR;
        else if (run.status == RUN_TIMEOUT)
            t->status = TEST_TIMEOUT;
        else if (run.status == RUN_OUTPUT_LIMIT)
            t->status = TEST_OUTPUT_LIMIT;
        else
            t->status = t->diff == 0 ? TEST_PASS : TEST_WRONG;

        if (t->status == TEST_PASS)
            r->passed++;
        else
            r->failed++;
        r->total_deduction += t->deduction;
        r->score += t->score;
        free(output);
    }

    /* 每一題都 TLE/OLE 沒有比對過，總差異數就無法計算 */
    for (i = 0; i < r->test_count; i++)
        if (r->tests[i].diff >= 0)
            break;
    if (i == r->test_count)
        r->total_diff = -1;

    /* Step 3：AI 修正過的程式再扣「修正扣分」(有下限)；總扣分 = 輸出扣分 + 修正扣分，但不會低於保底分 */
    if (r->status == STUDENT_CE_FIXED) {
        double output_score = r->score;
        r->score = config_ce_fixed_score(&g->cfg, output_score, r->fix_chars, &r->fix_penalty);
        r->ce_floor_applied = r->fix_penalty + 1e-9 < config_fix_penalty(&g->cfg, r->fix_chars);
        r->total_deduction = g->cfg.full_score - r->score;
    }
}

void grader_remove_temp(Grader *g)
{
    if (!g->cfg.keep_temp && g->temp_dir[0] != '\0')
        remove_dir_recursive(g->temp_dir);
}

void grader_close(Grader *g)
{
    int i;

    if (g->expected != NULL) {
        for (i = 0; i < g->tests.count; i++)
            free(g->expected[i]);
    }
    free(g->expected);
    free(g->expected_len);
    g->expected = NULL;
    g->expected_len = NULL;

    grader_remove_temp(g);

    testcase_free(&g->tests);
    string_list_free(&g->students);
    roster_free(&g->roster);
}

void reference_report_free(ReferenceReport *report)
{
    free(report->compile_log);
    free(report->checks);
    memset(report, 0, sizeof(*report));
}

void student_result_free(StudentResult *r)
{
    int i;
    for (i = 0; i < r->test_count; i++)
        free(r->tests[i].actual);
    free(r->tests);
    free(r->compile_log);
    free(r->fixed_source);
    free(r->fix_log);
    string_list_free(&r->sources);
    memset(r, 0, sizeof(*r));
}

void grader_find_student_sources(const char *student_dir, StringList *sources, char *relative, size_t size)
{
    relative[0] = '\0';
    if (file_exists(student_dir)) /* 學生交的是單一 .c 檔 (沒有資料夾) */
        string_list_add(sources, student_dir);
    else
        find_sources_nested(student_dir, 0, sources, relative, size);
}

/*
 * 學生清單 = 學生資料夾第一層的每個子資料夾；
 * 有些教學平台下載後是一堆散裝 .c 檔 (沒有子資料夾)，這種 .c 檔也各算一位學生，不能漏批。
 */
int grader_list_students(const char *students_dir, StringList *out)
{
    StringList all = {0}, files = {0};
    int i;

    list_dir(students_dir, "*", 1, &all);
    for (i = 0; i < all.count; i++)
        if (!is_ignored_folder(all.items[i]))
            string_list_add(out, all.items[i]);
    list_dir(students_dir, "*.c", 0, &files);
    for (i = 0; i < files.count; i++) {
        size_t len = strlen(files.items[i]);
        if (len > 2 && _stricmp(files.items[i] + len - 2, ".c") == 0)
            string_list_add(out, files.items[i]);
    }
    string_list_free(&all);
    string_list_free(&files);
    return out->count;
}
