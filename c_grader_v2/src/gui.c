/*
 * gui.c - 主視窗。
 *
 *   學生作業資料夾：[ D:/下載/作業1                          ] [選擇…] [學生名單…]
 *   題目 (參考答案 .c)：[ D:/題目/hw1.c                ] [選擇 .c…] [編輯測資…]
 *   快速套用模板：[建議：忽略格式小差異 + 區間扣分 ▼]   滿分 [100]  時間限制 [60] 秒
 *     (模板只是「一次填好下面的設定」，真正生效的永遠是下面的設定；
 *      下面改過之後，選單會自動顯示符合的模板或「自訂設定」，兩者不會衝突)
 *   ┌比對模式────┐┌比對選項──────────────┐┌評分方式──────────────────────┐
 *   │○ Strict    ││☑ 忽略行尾空白與結尾換行 ││○ 每 CHAR 扣分    每 1 CHAR 扣 [1] 分│
 *   │○ Ignore WS ││☑ 全形半形視為相同     ││○ 每 N CHAR 扣分  每 [3] CHAR 扣 [1]│
 *   │○ Token     ││☐ 忽略大小寫 ☐ 空白行  ││● 自訂區間        [編輯評分規則…]   │
 *   └────────────┘│忽略含以下文字的行 [   ]│└────────────────────────────────────┘
 *   ┌編譯失敗與執行異常處理──────────────────────────────────────────────────────┐
 *   │☑ 編譯失敗時交給本機 AI 做最小修正後繼續批改  工具 [auto▼] 每修正 1 CHAR 扣 [1] 分，上限 [0] 分│
 *   │☐ 程式當掉 (Runtime Error) 的題目直接 0 分   總扣分 = 輸出扣分 + 修正扣分                   │
 *   └──────────────────────────────────────────────────────────────────────────────┘
 *   [驗證參考答案] [▶ 開始批改] [匯出 CSV] [老師版報告] [學生版報告] [查看詳細] [查看原始碼] ▓░
 *   ┌資料夾┬姓名┬狀態┬通過┬差異數┬扣分┬成績┬備註────────┐
 *   狀態列 (視窗底部)
 *
 * 批改在背景執行緒 (worker) 進行，每批完一位學生就通知視窗更新，畫面不會卡住。
 * 每位學生批完會自動產生 HTML 比對報告 (題目資料夾/reports/)。
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "executor.h"
#include "exporter.h"
#include "gui.h"
#include "report.h"
#include "roster.h"
#include "util.h"

enum {
    ID_STUDENTS = 100,
    ID_BROWSE_STUDENTS,
    ID_ROSTER,
    ID_PROBLEM,
    ID_BROWSE_PROBLEM,
    ID_EDIT_TESTS,
    ID_TEMPLATE,
    ID_FULL_SCORE,
    ID_TIMEOUT,
    ID_CMP_STRICT,
    ID_CMP_WHITESPACE,
    ID_CMP_TOKEN,
    ID_OPT_TRAILING,
    ID_OPT_FULLWIDTH,
    ID_OPT_CASE,
    ID_OPT_BLANK,
    ID_OPT_KEYWORDS,
    ID_MODE_PER_CHAR,
    ID_MODE_PER_N,
    ID_MODE_RANGE,
    ID_PER_CHAR,
    ID_N_CHARS,
    ID_PER_N,
    ID_EDIT_RULES,
    ID_RULE_SUMMARY,
    ID_AI_FIX,
    ID_AI_TOOL,
    ID_AI_PER_CHAR,
    ID_AI_MAX,
    ID_AI_MIN,
    ID_AI_MAX_CHARS,
    ID_CE_FLOOR,
    ID_RE_ZERO,
    ID_VERIFY,
    ID_START,
    ID_EXPORT,
    ID_OPEN_REPORT,
    ID_STUDENT_REPORTS,
    ID_DETAIL,
    ID_VIEW_SOURCE,
    ID_PROGRESS,
    ID_STATUS,
    ID_LIST
};

/* 背景執行緒 -> 視窗 的訊息 */
enum {
    WM_APP_STATUS = WM_APP + 1, /* lParam = wchar_t* 狀態文字 (視窗負責 free) */
    WM_APP_STARTED,             /* wParam = 學生人數 */
    WM_APP_STUDENT_DONE,        /* wParam = 學生編號 */
    WM_APP_ASK_CONTINUE,        /* lParam = char* 參考程式驗證結果；回傳 1 = 繼續 */
    WM_APP_FINISHED             /* wParam = FINISH_*，lParam = char* 訊息 (可為 NULL，視窗負責 free) */
};

enum { FINISH_OK, FINISH_ERROR, FINISH_CANCELLED, FINISH_VERIFIED };

enum { COL_ID, COL_NAME, COL_STATUS, COL_PASSED, COL_DIFF, COL_PENALTY, COL_SCORE, COL_REMARKS, COL_COUNT };

/* AI 工具下拉選單的固定項目；之後若有自訂命令列會再加一項 */
static const char *AI_TOOL_NAMES[] = {"auto", "claude", "codex", "gemini"};
#define AI_TOOL_FIXED_COUNT 4

typedef struct {
    HWND wnd;
    HWND students, browse_students, roster, problem, browse_problem, edit_tests;
    HWND template_combo, full_score, timeout;
    HWND cmp[3];
    HWND opt_trailing, opt_fullwidth, opt_case, opt_blank, opt_keywords;
    HWND mode[3];
    HWND per_char, n_chars, per_n, edit_rules, rule_summary;
    HWND ai_fix, ai_tool, ai_per_char, ai_max, ai_min, ai_max_chars, ce_floor, re_zero, ai_hint;
    HWND verify, start, export_csv, open_report, student_reports, detail, view_source;
    HWND progress, status, list, tooltip;

    /* 面板 (取代群組框，在 WM_PAINT 自己畫)；座標以 96 DPI 為單位 */
    struct {
        RECT rc;
        const wchar_t *title;
    } panels[4];
    int panel_count;
    int panel_ce; /* 編譯失敗處理面板的索引 (寬度隨視窗) */

    GradeConfig cfg;            /* 畫面上的設定 (區間規則也在這裡) */
    char custom_ai_tool[256];   /* 設定檔裡的自訂 AI 命令列 (下拉選單第 5 項) */
    int loading;                /* 1 = 程式正在把設定填進畫面，不要觸發「自訂」判斷 */

    /* 最近一次批改的結果 (詳細畫面、匯出 CSV、報告會用到) */
    Grader grader;
    int has_grader;
    StudentResult *results;
    int student_count;
    int graded_count;
    char report_dir[GRADER_PATH_MAX];
    int has_report;

    /* 背景批改 */
    HANDLE worker;
    int busy;
    int verify_only;
    int closing;
    volatile LONG cancel;
    char run_problem[GRADER_PATH_MAX];
    char run_students[GRADER_PATH_MAX];
    GradeConfig run_cfg;

    int sort_column;
    int sort_ascending;
} App;

static App app;

/* ======================================================================
 * 小工具
 * ====================================================================== */

static char *get_text_utf8(HWND control)
{
    int n = GetWindowTextLengthW(control);
    wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
    char *s;
    size_t len;

    if (w == NULL)
        return _strdup("");
    GetWindowTextW(control, w, n + 1);
    s = wide_to_utf8(w);
    free(w);
    if (s == NULL)
        return _strdup("");
    /* 去掉前後空白與引號 (從檔案總管複製路徑時常會帶引號) */
    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '"'))
        s[--len] = '\0';
    while (s[0] == ' ' || s[0] == '"')
        memmove(s, s + 1, strlen(s));
    return s;
}

static void set_status(const wchar_t *text)
{
    gui_status_text(app.status, text);
}

static void set_status_utf8(const char *text)
{
    wchar_t *w = utf8_to_wide(text != NULL ? text : "");
    set_status(w != NULL ? w : L"");
    free(w);
}

static void set_number(HWND edit, double value)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%g", value);
    SetWindowTextW(edit, buf);
}

static int is_checked(HWND button)
{
    return SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void set_checked(HWND button, int checked)
{
    SendMessageW(button, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void lv_set_utf8(int row, int col, const char *text)
{
    wchar_t *w = utf8_to_wide(text);
    ListView_SetItemText(app.list, row, col, w != NULL ? w : L"");
    free(w);
}

static void open_path(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    if (w != NULL)
        ShellExecuteW(app.wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}

/* 找出某位學生目前在表格的第幾列 (表格可能被排序過) */
static int find_row(int index)
{
    LVFINDINFOW find;
    memset(&find, 0, sizeof(find));
    find.flags = LVFI_PARAM;
    find.lParam = index;
    return ListView_FindItem(app.list, -1, &find);
}

/* 批改完成的學生才算 (graded_count 只由視窗執行緒在收到 WM_APP_STUDENT_DONE 後更新，不會讀到批改中的半成品) */
static int is_graded(int index)
{
    return index >= 0 && index < app.graded_count && app.results != NULL;
}

/* 記住上次用的資料夾：%APPDATA%\c-grader\last_folders.txt */
static void last_folders_path(char *out, size_t size)
{
    wchar_t appdata[MAX_PATH];
    char *a;
    out[0] = '\0';
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0)
        return;
    a = wide_to_utf8(appdata);
    if (a == NULL)
        return;
    path_join(out, size, a, "c-grader");
    free(a);
    make_dir(out);
    path_join(out, size, out, "last_folders.txt");
}

static void save_last_folders(void)
{
    char path[GRADER_PATH_MAX], *students = get_text_utf8(app.students), *problem = get_text_utf8(app.problem);
    FILE *f;

    last_folders_path(path, sizeof(path));
    if (path[0] != '\0' && (f = fopen_utf8(path, "w")) != NULL) {
        fprintf(f, "%s\n%s\n", students, problem);
        fclose(f);
    }
    free(students);
    free(problem);
}

static void load_last_folders(void)
{
    char path[GRADER_PATH_MAX], line[GRADER_PATH_MAX];
    FILE *f;
    int i = 0;

    last_folders_path(path, sizeof(path));
    if (path[0] == '\0' || (f = fopen_utf8(path, "r")) == NULL)
        return;
    while (i < 2 && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        /* 第 1 行是學生資料夾，第 2 行是題目 (.c 檔或舊格式的資料夾) */
        if (line[0] != '\0' && (dir_exists(line) || (i == 1 && file_exists(line))))
            set_text_utf8(i == 0 ? app.students : app.problem, line);
        i++;
    }
    fclose(f);
}

/* ======================================================================
 * 設定 <-> 畫面
 * ====================================================================== */

static void update_rule_summary(void)
{
    StrBuf sb = {0};
    char pen[32];
    int i;

    sb_append(&sb, "區間：");
    for (i = 0; i < app.cfg.rules.range_count; i++) {
        const RangeRule *r = &app.cfg.rules.ranges[i];
        format_penalty(pen, sizeof(pen), r->penalty);
        if (strcmp(pen, "ALL") == 0)
            snprintf(pen, sizeof(pen), "扣光");
        if (r->max == RANGE_MAX)
            sb_appendf(&sb, "%s%ld+→%s", i ? "　" : "", r->min, pen);
        else if (r->min == r->max)
            sb_appendf(&sb, "%s%ld→%s", i ? "　" : "", r->min, pen);
        else
            sb_appendf(&sb, "%s%ld～%ld→%s", i ? "　" : "", r->min, r->max, pen);
    }
    if (app.cfg.rules.range_count == 0)
        sb_append(&sb, "(尚未設定)");
    set_text_utf8(app.rule_summary, sb.data);
    sb_free(&sb);
}

static void update_mode_controls(void)
{
    int idle = !app.busy, ai = is_checked(app.ai_fix);

    EnableWindow(app.per_char, idle && is_checked(app.mode[0]));
    EnableWindow(app.n_chars, idle && is_checked(app.mode[1]));
    EnableWindow(app.per_n, idle && is_checked(app.mode[1]));
    EnableWindow(app.edit_rules, idle && is_checked(app.mode[2]));
    EnableWindow(app.rule_summary, is_checked(app.mode[2]));
    EnableWindow(app.ai_tool, idle && ai);
    EnableWindow(app.ai_per_char, idle && ai);
    EnableWindow(app.ai_max, idle && ai);
    EnableWindow(app.ai_min, idle && ai);
    EnableWindow(app.ai_max_chars, idle && ai);
    EnableWindow(app.ce_floor, idle); /* 保底分不管有沒有 AI 都有效 */
}

static void refresh_template_match(void);

/* AI 工具下拉選單：固定 4 項 + (有自訂命令列時) 第 5 項 */
static void select_ai_tool(const char *tool)
{
    int i, count;

    for (i = 0; i < AI_TOOL_FIXED_COUNT; i++) {
        if (_stricmp(tool, AI_TOOL_NAMES[i]) == 0) {
            SendMessageW(app.ai_tool, CB_SETCURSEL, i, 0);
            return;
        }
    }
    /* 自訂命令列 */
    snprintf(app.custom_ai_tool, sizeof(app.custom_ai_tool), "%s", tool);
    count = (int)SendMessageW(app.ai_tool, CB_GETCOUNT, 0, 0);
    while (count > AI_TOOL_FIXED_COUNT)
        SendMessageW(app.ai_tool, CB_DELETESTRING, --count, 0);
    {
        char label[300];
        wchar_t *w;
        snprintf(label, sizeof(label), "自訂：%s", tool);
        w = utf8_to_wide(label);
        SendMessageW(app.ai_tool, CB_ADDSTRING, 0, (LPARAM)(w != NULL ? w : L"自訂"));
        free(w);
    }
    SendMessageW(app.ai_tool, CB_SETCURSEL, AI_TOOL_FIXED_COUNT, 0);
}

static void config_to_ui(const GradeConfig *cfg)
{
    int i;

    app.loading = 1; /* 填畫面時會觸發 EN_CHANGE，先不要判斷模板 */
    for (i = 0; i < 3; i++) {
        set_checked(app.cmp[i], (int)cfg->compare.mode == i);
        set_checked(app.mode[i], (int)cfg->rules.mode == i);
    }
    set_checked(app.opt_trailing, cfg->compare.ignore_trailing_space);
    set_checked(app.opt_fullwidth, cfg->compare.fullwidth_as_halfwidth);
    set_checked(app.opt_case, cfg->compare.ignore_case);
    set_checked(app.opt_blank, cfg->compare.ignore_blank_lines);
    set_text_utf8(app.opt_keywords, cfg->compare.ignore_lines_with);
    set_checked(app.re_zero, cfg->runtime_error_zero);

    set_checked(app.ai_fix, cfg->ai_fix);
    select_ai_tool(cfg->ai_fix_tool);
    set_number(app.ai_per_char, cfg->ai_fix_penalty_per_char);
    set_number(app.ai_max, cfg->ai_fix_penalty_max);
    set_number(app.ai_min, cfg->ai_fix_penalty_min_pct);
    set_number(app.ai_max_chars, cfg->ai_fix_max_chars);
    set_number(app.ce_floor, cfg->ce_score_floor_pct);

    set_number(app.full_score, cfg->full_score);
    set_number(app.timeout, cfg->timeout_ms / 1000.0);
    set_number(app.per_char, cfg->rules.per_char_penalty);
    set_number(app.n_chars, (double)cfg->rules.n_chars);
    set_number(app.per_n, cfg->rules.per_n_penalty);
    update_rule_summary();
    update_mode_controls();
    app.loading = 0;
    refresh_template_match();
}

/* 讀取畫面上的設定到 cfg；有錯誤時回傳 0 並寫入 err */
static int ui_to_config(GradeConfig *cfg, const wchar_t **err)
{
    double v;
    char *keywords;
    int sel;

    cfg->compare.mode = is_checked(app.cmp[1]) ? COMPARE_IGNORE_WHITESPACE
                      : is_checked(app.cmp[2]) ? COMPARE_TOKEN
                                               : COMPARE_STRICT;
    cfg->compare.ignore_trailing_space = is_checked(app.opt_trailing);
    cfg->compare.fullwidth_as_halfwidth = is_checked(app.opt_fullwidth);
    cfg->compare.ignore_case = is_checked(app.opt_case);
    cfg->compare.ignore_blank_lines = is_checked(app.opt_blank);
    keywords = get_text_utf8(app.opt_keywords);
    snprintf(cfg->compare.ignore_lines_with, sizeof(cfg->compare.ignore_lines_with), "%s", keywords);
    free(keywords);
    cfg->runtime_error_zero = is_checked(app.re_zero);

    cfg->rules.mode = is_checked(app.mode[0]) ? SCORE_PER_CHAR
                    : is_checked(app.mode[1]) ? SCORE_PER_N_CHAR
                                              : SCORE_RANGE;

    cfg->ai_fix = is_checked(app.ai_fix);
    sel = (int)SendMessageW(app.ai_tool, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < AI_TOOL_FIXED_COUNT)
        snprintf(cfg->ai_fix_tool, sizeof(cfg->ai_fix_tool), "%s", AI_TOOL_NAMES[sel]);
    else if (app.custom_ai_tool[0] != '\0')
        snprintf(cfg->ai_fix_tool, sizeof(cfg->ai_fix_tool), "%s", app.custom_ai_tool);
    else
        snprintf(cfg->ai_fix_tool, sizeof(cfg->ai_fix_tool), "auto");

    if (!read_number(app.full_score, &v) || v <= 0) {
        *err = L"滿分必須是大於 0 的數字。";
        return 0;
    }
    cfg->full_score = v;

    if (!read_number(app.timeout, &v) || v <= 0 || v > 600) {
        *err = L"時間限制必須介於 0～600 秒之間 (可以有小數，例如 0.5)。";
        return 0;
    }
    cfg->timeout_ms = (int)(v * 1000 + 0.5);

    if (read_number(app.per_char, &v) && v >= 0) {
        cfg->rules.per_char_penalty = v;
    } else if (cfg->rules.mode == SCORE_PER_CHAR) {
        *err = L"「每 1 CHAR 扣幾分」請輸入 0 以上的數字。";
        return 0;
    }

    if (read_number(app.n_chars, &v) && v >= 1 && v == (long)v) {
        cfg->rules.n_chars = (long)v;
    } else if (cfg->rules.mode == SCORE_PER_N_CHAR) {
        *err = L"「每 N CHAR」的 N 請輸入 1 以上的整數。";
        return 0;
    }

    if (read_number(app.per_n, &v) && v >= 0) {
        cfg->rules.per_n_penalty = v;
    } else if (cfg->rules.mode == SCORE_PER_N_CHAR) {
        *err = L"「每 N CHAR 扣幾分」請輸入 0 以上的數字。";
        return 0;
    }

    if (read_number(app.ai_per_char, &v) && v >= 0) {
        cfg->ai_fix_penalty_per_char = v;
    } else if (cfg->ai_fix) {
        *err = L"「每修正 1 CHAR 扣幾分」請輸入 0 以上的數字。";
        return 0;
    }
    if (read_number(app.ai_max, &v) && v >= 0) {
        cfg->ai_fix_penalty_max = v;
    } else if (cfg->ai_fix) {
        *err = L"「修正扣分上限」請輸入 0 以上的數字 (0 = 不設上限)。";
        return 0;
    }
    if (read_number(app.ai_min, &v) && v >= 0 && v <= 100) {
        cfg->ai_fix_penalty_min_pct = v;
    } else if (cfg->ai_fix) {
        *err = L"「修正扣分最少扣滿分的 %」請輸入 0～100。";
        return 0;
    }
    if (read_number(app.ai_max_chars, &v) && v >= 0 && v == (long)v) {
        cfg->ai_fix_max_chars = (int)v;
    } else if (cfg->ai_fix) {
        *err = L"「AI 修改超過幾個字不採用」請輸入 0 以上的整數 (0 = 不限)。";
        return 0;
    }
    if (read_number(app.ce_floor, &v) && v >= 0 && v <= 100) {
        cfg->ce_score_floor_pct = v;
    } else {
        *err = L"「編譯失敗保底分」請輸入 0～100 (滿分的百分比)。";
        return 0;
    }
    return 1;
}

/* ---------------- 模板：只是「一次填好下面的設定」 ---------------- */

/* 依畫面上目前的設定，讓下拉選單顯示符合的模板，或「自訂設定」 */
static void refresh_template_match(void)
{
    GradeConfig cfg = app.cfg;
    const wchar_t *err;
    int match;

    if (app.loading || app.template_combo == NULL)
        return;
    ui_to_config(&cfg, &err); /* 數字格式錯誤時仍用讀得到的部分判斷 */
    match = config_match_template(&cfg);
    SendMessageW(app.template_combo, CB_SETCURSEL, match >= 0 ? match : config_template_count(), 0);
}

static void on_template_selected(void)
{
    int index = (int)SendMessageW(app.template_combo, CB_GETCURSEL, 0, 0);
    const wchar_t *err;
    char msg[600], *p;

    if (index < 0 || index >= config_template_count()) {
        refresh_template_match(); /* 選「自訂設定」本身不做任何事 */
        return;
    }
    ui_to_config(&app.cfg, &err); /* 保留畫面上的滿分與時間限制 */
    config_apply_template(&app.cfg, index);
    config_to_ui(&app.cfg);
    snprintf(msg, sizeof(msg), "已把「%s」填入下方設定。%s", config_template_name(index),
             config_template_description(index));
    for (p = msg; *p != '\0'; p++)
        if (*p == '\n')
            *p = ' ';
    set_status_utf8(msg);
}

/* ---------------- 題目 (參考答案 .c) ---------------- */

/*
 * 目前畫面上的題目：參考答案 .c 與測資資料夾 (.c 旁邊的「<檔名>_測資」)。
 * 沒有選或找不到回傳 0。
 */
static int current_problem(char *reference, char *data_dir, size_t size)
{
    char *problem = get_text_utf8(app.problem);
    int ok = problem[0] != '\0' && (file_exists(problem) || dir_exists(problem));

    reference[0] = data_dir[0] = '\0';
    if (ok)
        grader_problem_paths(problem, reference, data_dir, size);
    free(problem);
    return ok;
}

/* ---------------- 學生清單預覽 (還沒批改也能查看原始碼) ---------------- */

static void free_previous_results(void);

static void fill_student_preview(const char *students_folder)
{
    char resolved[GRADER_PATH_MAX], roster_path[GRADER_PATH_MAX];
    StringList names = {0};
    Roster roster = {0};
    int i;

    free_previous_results();
    if (students_folder[0] == '\0' || !dir_exists(students_folder))
        return;
    roster_default_path(roster_path, sizeof(roster_path));
    roster_load(roster_path, &roster);

    grader_resolve_students_dir(students_folder, resolved, sizeof(resolved));
    grader_list_students(resolved, &names);
    for (i = 0; i < names.count; i++) {
        LVITEMW item;
        const RosterEntry *e = roster_find(&roster, names.items[i]);
        wchar_t *id = utf8_to_wide(names.items[i]);
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.lParam = i;
        item.pszText = id;
        ListView_InsertItem(app.list, &item);
        if (e != NULL)
            lv_set_utf8(i, COL_NAME, e->name);
        ListView_SetItemText(app.list, i, COL_STATUS, L"未批改");
        ListView_SetItemText(app.list, i, COL_REMARKS, L"雙擊查看原始碼");
        free(id);
    }
    string_list_free(&names);
    roster_free(&roster);
}

/* 檢查學生資料夾與題目並顯示在狀態列；測資資料夾有 grader.cfg 就載入 */
static void check_folders(int load_config)
{
    char *students = get_text_utf8(app.students);
    char reference[GRADER_PATH_MAX], data_dir[GRADER_PATH_MAX], path[GRADER_PATH_MAX];
    char resolved[GRADER_PATH_MAX], err[1024];
    StrBuf msg = {0};
    TestSet tests;
    StringList names = {0};

    if (!app.busy)
        fill_student_preview(students);

    if (students[0] == '\0') {
        sb_append(&msg, "⚠ 請選擇學生作業資料夾");
    } else if (!dir_exists(students)) {
        sb_append(&msg, "⚠ 找不到學生作業資料夾");
    } else {
        grader_resolve_students_dir(students, resolved, sizeof(resolved));
        grader_list_students(resolved, &names);
        sb_appendf(&msg, "學生 %d 位", names.count);
        string_list_free(&names);
    }

    if (!current_problem(reference, data_dir, sizeof(data_dir))) {
        sb_append(&msg, "　｜　⚠ 請選擇題目 (參考答案 .c 檔)");
    } else {
        path_join(path, sizeof(path), data_dir, "grader.cfg");
        if (load_config && file_exists(path)) {
            GradeConfig loaded = app.cfg;
            if (config_load(path, &loaded, err, sizeof(err))) {
                app.cfg = loaded;
                config_to_ui(&app.cfg);
                sb_append(&msg, "　｜　已載入這一題的設定");
            } else {
                sb_appendf(&msg, "　｜　⚠ grader.cfg 讀取失敗：%s", err);
            }
        }

        if (testcase_scan(data_dir, &tests, err, sizeof(err))) {
            int i, hidden = 0;
            for (i = 0; i < tests.count; i++)
                if (tests.items[i].hidden)
                    hidden++;
            sb_appendf(&msg, "　｜　測資 %d 組 (可見 %d、隱藏 %d)", tests.count, tests.count - hidden, hidden);
            testcase_free(&tests);
        } else {
            sb_append(&msg, "　｜　⚠ 還沒有測資，請按「編輯測資…」");
        }
        sb_appendf(&msg, "　｜　測資與報告存放於 %s", data_dir);
    }

    set_status_utf8(msg.data);
    sb_free(&msg);
    free(students);
}

static int CALLBACK browse_callback(HWND wnd, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data != 0)
        SendMessageW(wnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static int browse_folder(HWND edit, const wchar_t *title)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    wchar_t current[MAX_PATH] = L"", path[MAX_PATH];
    int ok = 0;

    GetWindowTextW(edit, current, MAX_PATH);
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = app.wnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_callback;
    bi.lParam = (LPARAM)(current[0] != L'\0' ? current : NULL);

    pidl = SHBrowseForFolderW(&bi);
    if (pidl == NULL)
        return 0;
    if (SHGetPathFromIDListW(pidl, path)) {
        SetWindowTextW(edit, path);
        ok = 1;
    }
    CoTaskMemFree(pidl);
    return ok;
}

/* 選擇題目：直接選參考答案 .c 檔 */
static int browse_reference(void)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH] = L"";

    GetWindowTextW(app.problem, file, MAX_PATH);
    if (file[0] != L'\0' && GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES)
        file[0] = L'\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.wnd;
    ofn.lpstrFilter = L"C 原始碼 (*.c)\0*.c\0所有檔案\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"選擇這一題的參考答案 (老師的標準程式 .c)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return 0;
    SetWindowTextW(app.problem, file);
    return 1;
}

static void on_edit_tests(void)
{
    char reference[GRADER_PATH_MAX], data_dir[GRADER_PATH_MAX];
    GradeConfig cfg = app.cfg;
    const wchar_t *err;

    if (!current_problem(reference, data_dir, sizeof(data_dir))) {
        MessageBoxW(app.wnd, L"請先選擇題目 (參考答案 .c 檔)。", L"編輯測資", MB_ICONINFORMATION);
        return;
    }
    ui_to_config(&cfg, &err); /* 預覽輸出時用畫面上的編譯設定與時間限制 */
    tests_dialog(app.wnd, reference, data_dir, &cfg);
    check_folders(0);
}

/* 產生 / 更新 名單.csv (放在批改工具資料夾，全班共用)，用 Excel 或記事本開啟讓老師填中文姓名 */
static void on_roster(void)
{
    char *students = get_text_utf8(app.students);
    char resolved[GRADER_PATH_MAX], path[GRADER_PATH_MAX];
    StringList names = {0};
    int added;

    if (students[0] == '\0' || !dir_exists(students)) {
        MessageBoxW(app.wnd, L"請先選擇學生作業資料夾。", L"學生名單", MB_ICONINFORMATION);
        free(students);
        return;
    }

    grader_resolve_students_dir(students, resolved, sizeof(resolved));
    grader_list_students(resolved, &names);
    roster_default_path(path, sizeof(path));
    added = roster_sync(path, &names);
    string_list_free(&names);
    free(students);
    if (added < 0) {
        MessageBoxW(app.wnd, L"無法寫入 名單.csv (是否正被 Excel 開啟？請先關閉再試)。", L"學生名單", MB_ICONERROR);
        return;
    }

    /* 自動配對：老師貼上教學平台的名單，英文名直接比對、中文名交給本機 AI 依拼音配對 */
    if (MessageBoxW(app.wnd,
                    L"下載後的資料夾名稱是姓名的拼音 (例如 張敦品 → zhang_dun_pin)。\n\n"
                    L"要貼上教學平台上的學生名單，讓工具自動配對中文姓名嗎？\n"
                    L"(英文名直接比對；中文名用本機的 claude / codex / gemini 依拼音配對)\n\n"
                    L"「是」= 貼上名單自動配對　「否」= 直接用 Excel 手動填",
                    L"學生名單", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        char *pasted = input_text_dialog(app.wnd, L"貼上教學平台的學生名單",
                                         L"在教學平台的繳交清單頁面，把學生名單整段選取、複製，貼在下面。\n"
                                         L"序號、繳交時間可以一起貼，每一行一位學生。");
        char script[GRADER_PATH_MAX];

        if (pasted != NULL && pasted[0] != '\0') {
            if (!tool_script_path("ai_roster.ps1", script, sizeof(script))) {
                MessageBoxW(app.wnd, L"找不到 tools\\ai_roster.ps1。", L"學生名單", MB_ICONERROR);
            } else {
                char tmp[GRADER_PATH_MAX], list_file[GRADER_PATH_MAX];
                StrBuf cmd = {0};

                make_run_temp_dir(tmp, sizeof(tmp));
                path_join(list_file, sizeof(list_file), tmp, "list.txt");
                write_file(list_file, pasted, strlen(pasted));
                sb_appendf(&cmd,
                           "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -Students \"%s\" "
                           "-List \"%s\" -Roster \"%s\" -Pause",
                           script, resolved, list_file, path);
                set_status(L"自動配對學生姓名中… (請看另外開啟的視窗)");
                if (run_visible_command(app.wnd, cmd.data) != 0)
                    MessageBoxW(app.wnd, L"自動配對沒有完成，請看視窗訊息，或改用 Excel 手動填。", L"學生名單",
                                MB_ICONWARNING);
                sb_free(&cmd);
                remove_dir_recursive(tmp);
            }
        }
        free(pasted);
    }

    open_path(path);
    MessageBoxW(app.wnd,
                L"已用 Excel (或記事本) 開啟 名單.csv (放在批改工具資料夾，每一題共用)，請檢查一次：\n\n"
                L"• A 欄「資料夾名稱」不要改 (下載後的英文+底線名稱)\n"
                L"• B 欄中文姓名 (自動配對的結果；空白的請手動補)\n"
                L"• C 欄學號：能從資料夾名稱看出來的已自動填好\n\n"
                L"填好後「存檔」並關閉 Excel (格式保持 CSV)，再按這裡的「確定」。",
                L"學生名單", MB_ICONINFORMATION);
    check_folders(0);
}

static void on_edit_rules(void)
{
    if (rules_dialog(app.wnd, &app.cfg.rules)) {
        update_rule_summary();
        refresh_template_match();
    }
}

/* ======================================================================
 * 結果表格
 * ====================================================================== */

static void fill_row(int index)
{
    const StudentResult *r = &app.results[index];
    char buf[64], deduction[32], score[32];
    StrBuf remarks = {0};
    int row = find_row(index), j;

    if (row < 0)
        return;

    format_score(score, sizeof(score), r->score);
    format_score(deduction, sizeof(deduction), r->total_deduction);

    if (student_ran_tests(r->status)) {
        lv_set_utf8(row, COL_STATUS, student_status_text(r->status));
        snprintf(buf, sizeof(buf), "%d/%d", r->passed, r->test_count);
        lv_set_utf8(row, COL_PASSED, buf);
        if (r->total_diff >= 0)
            snprintf(buf, sizeof(buf), "%ld", r->total_diff);
        else
            snprintf(buf, sizeof(buf), "-");
        lv_set_utf8(row, COL_DIFF, buf);
        lv_set_utf8(row, COL_PENALTY, deduction);
        for (j = 0; j < r->test_count; j++) {
            TestStatus s = r->tests[j].status;
            if (s == TEST_RUNTIME_ERROR || s == TEST_TIMEOUT || s == TEST_OUTPUT_LIMIT)
                sb_appendf(&remarks, "%s%s:%s", remarks.len ? "  " : "", app.grader.tests.items[j].name,
                           test_status_name(s));
        }
    } else {
        lv_set_utf8(row, COL_STATUS, r->status == STUDENT_COMPILE_ERROR ? "CE 編譯失敗" : "沒有 .c 檔");
        snprintf(buf, sizeof(buf), "0/%d", r->test_count);
        lv_set_utf8(row, COL_PASSED, buf);
        lv_set_utf8(row, COL_DIFF, "-");
        lv_set_utf8(row, COL_PENALTY, "-");
        if (r->status == STUDENT_COMPILE_ERROR)
            sb_append(&remarks, "雙擊查看編譯錯誤");
    }
    if (r->note[0] != '\0')
        sb_appendf(&remarks, "%s※%s", remarks.len ? "  " : "", r->note);
    lv_set_utf8(row, COL_NAME, r->name);
    lv_set_utf8(row, COL_SCORE, score);
    lv_set_utf8(row, COL_REMARKS, remarks.data != NULL ? remarks.data : "");
    sb_free(&remarks);
}

static int compare_numbers(double a, double b)
{
    return (a > b) - (a < b);
}

static int CALLBACK sort_rows(LPARAM a, LPARAM b, LPARAM param)
{
    const StudentResult *x = &app.results[a], *y = &app.results[b];
    int gx = is_graded((int)a), gy = is_graded((int)b);
    int result = 0;
    (void)param;

    if (gx != gy)
        return gx ? -1 : 1; /* 還沒批改的永遠排在最後 */
    if (gx) {
        switch (app.sort_column) {
        case COL_NAME:    result = strcmp(x->name, y->name); break;
        case COL_STATUS:  result = compare_numbers(x->status, y->status); break;
        case COL_PASSED:  result = compare_numbers(x->passed, y->passed); break;
        case COL_DIFF:    result = compare_numbers(x->total_diff, y->total_diff); break;
        case COL_PENALTY: result = compare_numbers(x->total_deduction, y->total_deduction); break;
        case COL_SCORE:   result = compare_numbers(x->score, y->score); break;
        default:          break;
        }
    }
    if (result == 0)
        result = strcmp(app.grader.students.items[a], app.grader.students.items[b]);
    return app.sort_ascending ? result : -result;
}

static void on_column_click(int column)
{
    if (app.results == NULL || !app.has_grader)
        return;
    if (column == app.sort_column)
        app.sort_ascending = !app.sort_ascending;
    else {
        app.sort_column = column;
        app.sort_ascending = column == COL_ID || column == COL_NAME || column == COL_STATUS; /* 分數類預設由高到低 */
    }
    ListView_SortItems(app.list, sort_rows, 0);
    gui_set_sort_arrow(app.list, column, app.sort_ascending);
}

static int selected_student(void)
{
    int row = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
    LVITEMW item;

    if (row < 0)
        return -1;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = row;
    ListView_GetItem(app.list, &item);
    return (int)item.lParam;
}

static void student_report_path(int index, char *out, size_t size)
{
    char name[300];
    report_file_name(&app.results[index], name, sizeof(name));
    path_join(out, size, app.report_dir, name);
}

/* 調閱學生原始碼：還沒批改也可以看 (直接讀學生資料夾，不會修改任何檔案) */
static void on_view_source(void)
{
    int row = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
    wchar_t folder_w[256] = L"", name_w[128] = L"", title[512];
    char *folder, *students, resolved[GRADER_PATH_MAX], dir[GRADER_PATH_MAX], relative[GRADER_PATH_MAX], *text;
    StringList sources = {0};

    if (row < 0) {
        MessageBoxW(app.wnd, L"請先在表格中選一位學生。", L"查看原始碼", MB_ICONINFORMATION);
        return;
    }
    ListView_GetItemText(app.list, row, COL_ID, folder_w, 256);
    ListView_GetItemText(app.list, row, COL_NAME, name_w, 128);
    folder = wide_to_utf8(folder_w);

    if (app.has_grader) {
        snprintf(resolved, sizeof(resolved), "%s", app.grader.students_dir);
    } else {
        students = get_text_utf8(app.students);
        grader_resolve_students_dir(students, resolved, sizeof(resolved));
        free(students);
    }
    path_join(dir, sizeof(dir), resolved, folder != NULL ? folder : "");
    grader_find_student_sources(dir, &sources, relative, sizeof(relative));
    text = source_listing_text(&sources);

    if (name_w[0] != L'\0')
        swprintf(title, 512, L"原始碼 - %ls (%ls)", name_w, folder_w);
    else
        swprintf(title, 512, L"原始碼 - %ls", folder_w);
    show_text_window(app.wnd, title, text);

    free(text);
    free(folder);
    string_list_free(&sources);
}

static void on_detail(void)
{
    int index = selected_student();
    const StudentResult *r;
    wchar_t title[300], *id;
    char report_path[GRADER_PATH_MAX] = "";

    if (index < 0) {
        MessageBoxW(app.wnd, L"請先在表格中選一位學生。", L"查看詳細", MB_ICONINFORMATION);
        return;
    }
    if (!is_graded(index)) {
        on_view_source(); /* 還沒批改：先看原始碼 */
        return;
    }
    r = &app.results[index];
    if (app.has_report)
        student_report_path(index, report_path, sizeof(report_path));

    if (r->status == STUDENT_COMPILE_ERROR) {
        StrBuf sb = {0};
        id = utf8_to_wide(r->id);
        swprintf(title, 300, L"%ls - Compile Error (gcc 錯誤訊息)", id != NULL ? id : L"");
        free(id);
        sb_append(&sb, r->compile_log != NULL ? r->compile_log : "");
        if (r->note[0] != '\0')
            sb_appendf(&sb, "\n\n[批改工具] %s\n", r->note);
        if (student_has_fix(r)) {
            char *fix = student_fix_text(r);
            sb_appendf(&sb, "\n==================== AI (%s) 的修正 (%s) ====================\n", r->fix_tool,
                       r->fix_rejected ? "改太多，不採用" : "仍無法編譯");
            sb_append(&sb, fix);
            free(fix);
            if (r->fix_log != NULL && r->fix_log[0] != '\0')
                sb_appendf(&sb, "\n---- 修正後的 gcc 訊息 ----\n%s", r->fix_log);
        }
        show_text_window(app.wnd, title, sb.data);
        sb_free(&sb);
    } else if (r->status == STUDENT_NO_SOURCE) {
        MessageBoxW(app.wnd, L"這位學生的資料夾裡找不到任何 .c 檔 (也找過子資料夾)，成績為 0。", L"查看詳細",
                    MB_ICONINFORMATION);
    } else {
        detail_dialog(app.wnd, &app.grader, r, report_path);
    }
}

static void on_open_report(void)
{
    char path[GRADER_PATH_MAX];
    int index = selected_student();

    if (!app.has_report) {
        MessageBoxW(app.wnd, L"還沒有比對報告。按「開始批改」後會自動產生。", L"比對報告", MB_ICONINFORMATION);
        return;
    }
    /* 老師版：有選學生就開他的報告，沒選就開全班總表 */
    if (is_graded(index))
        student_report_path(index, path, sizeof(path));
    else
        path_join(path, sizeof(path), app.report_dir, "index.html");
    open_path(path);
}

/* 學生版：打開資料夾，每位學生一個 .html，可以直接寄給 / 上傳給學生 */
static void on_student_reports(void)
{
    char dir[GRADER_PATH_MAX];

    if (!app.has_report) {
        MessageBoxW(app.wnd, L"還沒有報告。按「開始批改」後會自動產生。", L"學生版報告", MB_ICONINFORMATION);
        return;
    }
    report_default_dir(&app.grader, REPORT_STUDENT, dir, sizeof(dir));
    open_path(dir);
}

static void on_export(void)
{
    OPENFILENAMEW ofn;
    wchar_t path[MAX_PATH] = L"";
    char *path_utf8, err[1024], def[GRADER_PATH_MAX];
    wchar_t *w;

    if (app.graded_count == 0) {
        MessageBoxW(app.wnd, L"還沒有批改結果可以匯出。", L"匯出 CSV", MB_ICONINFORMATION);
        return;
    }

    path_join(def, sizeof(def), app.grader.problem_dir, "result.csv");
    w = utf8_to_wide(def);
    if (w != NULL) {
        wcsncpy(path, w, MAX_PATH - 1);
        free(w);
    }

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.wnd;
    ofn.lpstrFilter = L"CSV 檔 (*.csv)\0*.csv\0所有檔案\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn))
        return;

    path_utf8 = wide_to_utf8(path);
    if (export_csv(path_utf8, &app.grader, app.results, app.graded_count, err, sizeof(err))) {
        wchar_t msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"已匯出 %d 位學生的成績：\n%ls", app.graded_count, path);
        MessageBoxW(app.wnd, msg, L"匯出 CSV", MB_ICONINFORMATION);
    } else {
        w = utf8_to_wide(err);
        MessageBoxW(app.wnd, w != NULL ? w : L"匯出失敗", L"匯出 CSV", MB_ICONERROR);
        free(w);
    }
    free(path_utf8);
}

/* ======================================================================
 * 背景批改
 * ====================================================================== */

static void post_status(const char *text)
{
    PostMessageW(app.wnd, WM_APP_STATUS, 0, (LPARAM)utf8_to_wide(text));
}

static void post_finished(int kind, char *text)
{
    PostMessageW(app.wnd, WM_APP_FINISHED, kind, (LPARAM)text);
}

static char *reference_report_text(const Grader *g, const ReferenceReport *report)
{
    StrBuf sb = {0};
    int i;

    sb_append(&sb, "參考程式編譯成功。\n\n");
    for (i = 0; i < report->count; i++) {
        const ReferenceCheck *c = &report->checks[i];
        if (!c->has_out)
            sb_appendf(&sb, "%-12s 沒有 .out，使用參考程式輸出作為標準答案\n", c->name);
        else if (c->run_status != RUN_OK)
            sb_appendf(&sb, "%-12s ⚠ 參考程式執行失敗：%s\n", c->name, run_status_name(c->run_status));
        else if (c->diff != 0)
            sb_appendf(&sb, "%-12s ⚠ 參考程式輸出與 .out 不一致 (差異 %ld CHAR)\n", c->name, c->diff);
        else
            sb_appendf(&sb, "%-12s ✔ 一致\n", c->name);
    }
    if (report->mismatches > 0 || report->failures > 0)
        sb_append(&sb, "\n⚠ 參考程式輸出與 testcase 標準答案不一致\n(批改時以 .out 為標準答案)\n");
    else
        sb_append(&sb, "\n✔ 參考答案可以使用。\n");

    sb_append(&sb, "\n---- 各 testcase 的標準答案 ----\n");
    for (i = 0; i < g->tests.count; i++) {
        sb_appendf(&sb, "\n[%s]\n", g->tests.items[i].name);
        if (g->expected[i] != NULL) {
            size_t len = g->expected_len[i] > 4000 ? 4000 : g->expected_len[i];
            sb_append_len(&sb, g->expected[i], len);
        }
    }

    if (report->compile_log != NULL && report->compile_log[0] != '\0')
        sb_appendf(&sb, "\n\n---- 編譯警告 ----\n%s", report->compile_log);
    return sb.data;
}

static DWORD WINAPI worker_main(LPVOID param)
{
    char err[2048] = "";
    ReferenceReport report;
    char *text;
    int i, n, report_errors = 0;
    (void)param;

    post_status("讀取資料夾…");
    if (!grader_open(&app.grader, app.run_problem, app.run_students, &app.run_cfg, err, sizeof(err))) {
        post_finished(FINISH_ERROR, _strdup(err));
        return 0;
    }
    app.has_grader = 1;
    n = app.grader.students.count;
    app.results = calloc(n > 0 ? n : 1, sizeof(StudentResult));
    app.student_count = n;

    post_status("編譯參考答案並執行所有 testcase…");
    if (!grader_prepare_reference(&app.grader, &report, err, sizeof(err))) {
        StrBuf sb = {0};
        sb_appendf(&sb, "%s\n", err);
        if (report.compile_log != NULL && report.compile_log[0] != '\0')
            sb_appendf(&sb, "\n---- compiler error ----\n%s", report.compile_log);
        reference_report_free(&report);
        grader_remove_temp(&app.grader);
        post_finished(FINISH_ERROR, sb.data);
        return 0;
    }

    text = reference_report_text(&app.grader, &report);
    if (app.verify_only) {
        reference_report_free(&report);
        grader_remove_temp(&app.grader);
        post_finished(FINISH_VERIFIED, text);
        return 0;
    }
    if (report.mismatches > 0 || report.failures > 0) {
        /* SendMessage 會等老師在視窗上回答完才返回 */
        if (!SendMessageW(app.wnd, WM_APP_ASK_CONTINUE, 0, (LPARAM)text)) {
            free(text);
            reference_report_free(&report);
            grader_remove_temp(&app.grader);
            post_finished(FINISH_CANCELLED, NULL);
            return 0;
        }
    }
    free(text);
    reference_report_free(&report);

    report_default_dir(&app.grader, REPORT_TEACHER, app.report_dir, sizeof(app.report_dir));
    PostMessageW(app.wnd, WM_APP_STARTED, n, 0);
    for (i = 0; i < n; i++) {
        char msg[512];
        if (app.cancel)
            break;
        snprintf(msg, sizeof(msg), "批改中 (%d/%d)：%s%s", i + 1, n, app.grader.students.items[i],
                 app.run_cfg.ai_fix ? "　(編譯失敗的學生會呼叫本機 AI 修正，可能需要一兩分鐘)" : "");
        post_status(msg);
        grader_grade_student(&app.grader, i, &app.results[i]);
        if (!report_write_both(&app.grader, &app.results[i], err, sizeof(err))) /* 老師版 + 學生版 */
            report_errors++;
        PostMessageW(app.wnd, WM_APP_STUDENT_DONE, i, 0);
    }

    post_status("產生全班總表並清除暫存檔…");
    report_write_teacher_index(&app.grader, app.results, app.cancel ? i : n, err, sizeof(err));
    app.has_report = 1;
    grader_remove_temp(&app.grader);
    if (report_errors > 0)
        post_finished(FINISH_ERROR, _strdup("批改完成，但有部分比對報告無法寫入 (reports 資料夾是否被鎖定？)"));
    else
        post_finished(app.cancel ? FINISH_CANCELLED : FINISH_OK, NULL);
    return 0;
}

static void update_busy_ui(void)
{
    HWND idle_only[] = {app.students, app.browse_students, app.roster, app.problem, app.browse_problem,
                        app.edit_tests, app.template_combo, app.full_score, app.timeout,
                        app.cmp[0], app.cmp[1], app.cmp[2], app.opt_trailing, app.opt_fullwidth, app.opt_case,
                        app.opt_blank, app.opt_keywords, app.mode[0], app.mode[1], app.mode[2], app.re_zero, app.ce_floor,
                        app.ai_fix, app.verify, app.export_csv, app.open_report, app.student_reports};
    int idle = !app.busy;
    size_t i;

    for (i = 0; i < sizeof(idle_only) / sizeof(idle_only[0]); i++)
        EnableWindow(idle_only[i], idle);
    SetWindowTextW(app.start, app.busy && !app.verify_only ? L"■ 停止批改" : L"▶ 開始批改");
    EnableWindow(app.start, idle || !app.verify_only);
    update_mode_controls();
}

static void free_previous_results(void)
{
    int i;

    ListView_DeleteAllItems(app.list);
    gui_set_sort_arrow(app.list, -1, 1);
    if (app.results != NULL) {
        for (i = 0; i < app.student_count; i++)
            student_result_free(&app.results[i]);
        free(app.results);
    }
    app.results = NULL;
    app.student_count = 0;
    app.graded_count = 0;
    app.has_report = 0;
    if (app.has_grader)
        grader_close(&app.grader);
    app.has_grader = 0;
}

static void start_run(int verify_only)
{
    const wchar_t *err = NULL;
    char *students, *problem, cfg_path[GRADER_PATH_MAX], msg[4096];
    char reference[GRADER_PATH_MAX], data_dir[GRADER_PATH_MAX];
    GradeConfig cfg = app.cfg;

    if (app.busy) {
        if (!app.verify_only) {
            InterlockedExchange(&app.cancel, 1);
            set_status(L"停止中… (等目前這位學生批改完)");
            EnableWindow(app.start, FALSE);
        }
        return;
    }

    students = get_text_utf8(app.students);
    problem = get_text_utf8(app.problem);
    if (!current_problem(reference, data_dir, sizeof(data_dir))) {
        MessageBoxW(app.wnd, L"請先選擇題目 (參考答案 .c 檔)。", L"C 作業批改工具", MB_ICONWARNING);
        goto done;
    }
    if (!file_exists(reference) && !dir_exists(reference)) {
        MessageBoxW(app.wnd, L"找不到參考答案 .c 檔。", L"C 作業批改工具", MB_ICONWARNING);
        goto done;
    }
    if (!verify_only && (students[0] == '\0' || !dir_exists(students))) {
        MessageBoxW(app.wnd, L"請先選擇學生作業資料夾 (每個子資料夾是一位學生)。", L"C 作業批改工具",
                    MB_ICONWARNING);
        goto done;
    }
    if (!ui_to_config(&cfg, &err)) {
        MessageBoxW(app.wnd, err, L"設定有誤", MB_ICONWARNING);
        goto done;
    }

    if (!verify_only) {
        RulesCheck check = score_validate(&cfg.rules, msg, sizeof(msg));
        if (check != RULES_OK) {
            wchar_t *w = utf8_to_wide(msg), text[4600];
            if (check == RULES_ERROR) {
                swprintf(text, 4600, L"評分規則有錯誤，無法批改：\n\n%ls", w != NULL ? w : L"");
                MessageBoxW(app.wnd, text, L"評分規則", MB_ICONERROR);
            } else {
                swprintf(text, 4600, L"%ls\n仍要開始批改嗎？", w != NULL ? w : L"");
                check = MessageBoxW(app.wnd, text, L"評分規則", MB_YESNO | MB_ICONWARNING) == IDYES ? RULES_OK
                                                                                                    : RULES_ERROR;
            }
            free(w);
            if (check == RULES_ERROR)
                goto done;
        }
        /* CE 自動修正有開，但這台電腦找不到 AI 工具：先講清楚，老師可以決定要不要繼續 */
        if (cfg.ai_fix) {
            char name[AI_TOOL_NAME_MAX], command[AI_COMMAND_MAX];
            if (!ai_tool_find(cfg.ai_fix_tool, name, sizeof(name), command, sizeof(command))) {
                if (MessageBoxW(app.wnd,
                                L"已勾選「編譯失敗時交給本機 AI 做最小修正」，但這台電腦找不到 claude / codex / gemini "
                                L"命令列工具。\n\n編譯失敗的學生會直接給保底分。仍要開始批改嗎？",
                                L"找不到 AI 工具", MB_YESNO | MB_ICONWARNING) != IDYES)
                    goto done;
            }
        }
    }

    /* 設定自動存到 測資資料夾\grader.cfg，下次選同一題會自動載入 */
    app.cfg = cfg;
    make_dir(data_dir);
    path_join(cfg_path, sizeof(cfg_path), data_dir, "grader.cfg");
    config_save(cfg_path, &app.cfg);
    save_last_folders();

    free_previous_results();
    snprintf(app.run_problem, sizeof(app.run_problem), "%s", problem);
    snprintf(app.run_students, sizeof(app.run_students), "%s", students); /* 驗證參考答案時可以是空的 */
    app.run_cfg = app.cfg;
    app.verify_only = verify_only;
    app.cancel = 0;
    app.busy = 1;
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    update_busy_ui();

    app.worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (app.worker == NULL) {
        app.busy = 0;
        update_busy_ui();
        MessageBoxW(app.wnd, L"無法啟動批改執行緒。", L"C 作業批改工具", MB_ICONERROR);
    }
done:
    free(students);
    free(problem);
}

static void on_started(int count)
{
    int i;

    ListView_DeleteAllItems(app.list);
    for (i = 0; i < count; i++) {
        LVITEMW item;
        wchar_t *id = utf8_to_wide(app.grader.students.items[i]);
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.lParam = i;
        item.pszText = id;
        ListView_InsertItem(app.list, &item);
        {
            const RosterEntry *e = roster_find(&app.grader.roster, app.grader.students.items[i]);
            if (e != NULL)
                lv_set_utf8(i, COL_NAME, e->name);
        }
        ListView_SetItemText(app.list, i, COL_STATUS, L"等待中");
        free(id);
    }
    SendMessageW(app.progress, PBM_SETRANGE32, 0, count > 0 ? count : 1);
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
}

static void on_student_done(int index)
{
    int row;

    app.graded_count = index + 1;
    fill_row(index);
    SendMessageW(app.progress, PBM_SETPOS, index + 1, 0);
    row = find_row(index);
    if (row >= 0) {
        ListView_EnsureVisible(app.list, row, FALSE);
        ListView_RedrawItems(app.list, row, row);
    }
}

static void show_summary(int cancelled)
{
    StrBuf sb = {0};
    double total = 0;
    int i, full = 0, ce = 0, fixed = 0, problems = 0;
    char avg[32];

    for (i = 0; i < app.graded_count; i++) {
        const StudentResult *r = &app.results[i];
        int j;
        total += r->score;
        if (!student_ran_tests(r->status))
            ce++;
        else if (r->passed == r->test_count)
            full++;
        if (r->status == STUDENT_CE_FIXED)
            fixed++;
        for (j = 0; j < r->test_count; j++)
            if (r->tests[j].status == TEST_RUNTIME_ERROR || r->tests[j].status == TEST_TIMEOUT ||
                r->tests[j].status == TEST_OUTPUT_LIMIT) {
                problems++;
                break;
            }
    }
    format_score(avg, sizeof(avg), app.graded_count > 0 ? total / app.graded_count : 0);
    sb_appendf(&sb, "%s：%d 位　平均 %s 分　全對 %d 位　編譯失敗/沒檔案 %d 位　AI 修正編譯錯誤 %d 位　執行異常 %d 位"
                    "　｜　比對報告已產生，按「老師版報告」開啟",
               cancelled ? "已停止" : "✔ 批改完成", app.graded_count, avg, full, ce, fixed, problems);
    set_status_utf8(sb.data);
    sb_free(&sb);
}

static void on_finished(int kind, char *text)
{
    if (app.worker != NULL) {
        WaitForSingleObject(app.worker, INFINITE);
        CloseHandle(app.worker);
        app.worker = NULL;
    }
    app.busy = 0;
    update_busy_ui();

    if (app.closing) {
        free(text);
        DestroyWindow(app.wnd);
        return;
    }

    switch (kind) {
    case FINISH_OK:
        show_summary(0);
        break;
    case FINISH_CANCELLED:
        if (app.graded_count > 0)
            show_summary(1);
        else
            set_status(L"已取消批改。");
        break;
    case FINISH_VERIFIED:
        set_status(L"參考答案驗證完成。");
        show_text_window(app.wnd, L"驗證參考答案", text);
        break;
    case FINISH_ERROR:
        if (app.graded_count > 0)
            show_summary(0);
        else
            set_status(L"⚠ 批改失敗，請看錯誤訊息。");
        if (text != NULL && strchr(text, '\n') != NULL && strlen(text) > 120) {
            show_text_window(app.wnd, L"錯誤", text);
        } else {
            wchar_t *w = utf8_to_wide(text != NULL ? text : "未知錯誤");
            MessageBoxW(app.wnd, w, L"錯誤", MB_ICONERROR);
            free(w);
        }
        break;
    }
    free(text);
}

static LRESULT on_ask_continue(const char *report)
{
    wchar_t *w, *text;
    size_t size;
    int answer;

    if (app.cancel || app.closing)
        return 0;
    w = utf8_to_wide(report);
    size = (w != NULL ? wcslen(w) : 0) + 128;
    text = malloc(size * sizeof(wchar_t));
    if (text == NULL) {
        free(w);
        return 0;
    }
    swprintf(text, size, L"%ls\n是否繼續批改學生程式？", w != NULL ? w : L"");
    answer = MessageBoxW(app.wnd, text, L"⚠ 參考程式輸出與 testcase 標準答案不一致", MB_YESNO | MB_ICONWARNING);
    free(text);
    free(w);
    return answer == IDYES;
}

/* ======================================================================
 * 建立畫面
 * ====================================================================== */

static HWND add_label(HWND parent, const wchar_t *text, int x, int y, int w, int h)
{
    return make_control(parent, L"STATIC", text, SS_LEFT, 0, x, y, w, h, -1);
}

static HWND add_edit(HWND parent, int x, int y, int w, int id)
{
    return make_control(parent, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, WS_EX_CLIENTEDGE, x, y, w,
                        24, id);
}

static HWND add_button(HWND parent, const wchar_t *text, int x, int y, int w, int h, int id)
{
    return make_control(parent, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, x, y, w, h, id);
}

static HWND add_check(HWND parent, const wchar_t *text, int x, int y, int w, int id)
{
    return make_control(parent, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 0, x, y, w, 22, id);
}

/* 面板不是控制項：記下矩形與標題，WM_PAINT 時畫在子元件底下 */
static int add_group(HWND parent, const wchar_t *text, int x, int y, int w, int h)
{
    int i = app.panel_count++;
    (void)parent;
    app.panels[i].rc.left = x;
    app.panels[i].rc.top = y;
    app.panels[i].rc.right = x + w;
    app.panels[i].rc.bottom = y + h;
    app.panels[i].title = text;
    return i;
}

static void paint_panels(HWND wnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    int i;

    for (i = 0; i < app.panel_count; i++) {
        RECT rc = {dpi(app.panels[i].rc.left), dpi(app.panels[i].rc.top), dpi(app.panels[i].rc.right),
                   dpi(app.panels[i].rc.bottom)};
        gui_draw_panel(dc, &rc, app.panels[i].title);
    }
    EndPaint(wnd, &ps);
}

static void create_tooltips(void)
{
    app.tooltip = gui_create_tooltip(app.wnd);
    gui_add_tooltip(app.tooltip, app.template_combo,
                    L"模板只是一次把下方的比對方式與評分方式填好；下方改過之後，選單會自動顯示符合的模板或「自訂設定」。");
    gui_add_tooltip(app.tooltip, app.timeout, L"每個 testcase 的時間限制。小於 10 秒時，第一次超時會重跑一次確認。");
    gui_add_tooltip(app.tooltip, app.opt_trailing,
                    L"每一行結尾的空白、Tab、全形空白，以及整個輸出最後多或少的換行，都不算差異。");
    gui_add_tooltip(app.tooltip, app.opt_fullwidth, L"全形英數與標點 (：、１、Ａ) 視為半形；全形空白視為半形空白。");
    gui_add_tooltip(app.tooltip, app.opt_case, L"只影響英文字母 A-Z 與 a-z。");
    gui_add_tooltip(app.tooltip, app.opt_blank, L"整行只有空白的行不比對。");
    gui_add_tooltip(app.tooltip, app.opt_keywords,
                    L"含這些文字的整行不比對 (標準答案與學生輸出同樣處理)。多個關鍵字用 | 分隔，例如：請輸入|請依序");
    gui_add_tooltip(app.tooltip, app.ai_fix,
                    L"編譯失敗的學生：把原始碼與 gcc 錯誤訊息交給本機 AI 做最小修改，修得好就用修正後的程式執行測資。"
                    L"總扣分 = 各題輸出扣分 + 修正扣分。找不到 AI 工具、修不好或改太多時，給編譯失敗保底分。");
    gui_add_tooltip(app.tooltip, app.ai_tool, L"auto = 依序找本機的 claude → codex → gemini 命令列工具。");
    gui_add_tooltip(app.tooltip, app.ai_per_char,
                    L"修正字元數 = 原始碼 → 修正後原始碼 的編輯距離 (中文字 = 1 CHAR)。修正扣分 = 修正字元數 × 這個數字。");
    gui_add_tooltip(app.tooltip, app.ai_max, L"修正扣分的上限；0 = 不另設上限 (最多仍只扣到滿分)。");
    gui_add_tooltip(app.tooltip, app.re_zero, L"不勾選時，程式當掉前已經印出的內容仍照常比對計分。");
    gui_add_tooltip(app.tooltip, app.ai_min,
                    L"AI 修好的學生至少扣滿分的這個百分比：沒有先編譯成功本身就有代價 (少一個分號也不會只扣 1 分)。");
    gui_add_tooltip(app.tooltip, app.ai_max_chars,
                    L"AI 修改超過這麼多字就不採用 (可能順手修好了邏輯)，改給保底分並在報告標示請老師確認。0 = 不限。");
    gui_add_tooltip(app.tooltip, app.ce_floor,
                    L"只要是編譯失敗，成績一律不低於這個分數：AI 修不好、改太多、找不到工具時直接給這個分數；"
                    L"AI 修好時，扣完修正扣分與輸出扣分後也不會低於這個分數。");
    gui_add_tooltip(app.tooltip, app.verify, L"只編譯參考答案並執行所有測資，檢查標準答案是否合理，不批改學生。");
    gui_add_tooltip(app.tooltip, app.start, L"批改所有學生，並自動產生老師版與學生版 HTML 報告。");
}

static void create_controls(HWND wnd)
{
    static const wchar_t *columns[COL_COUNT] = {L"資料夾名稱", L"姓名", L"狀態", L"通過", L"差異數",
                                                L"扣分",       L"成績", L"備註"};
    static const int widths[COL_COUNT] = {170, 90, 100, 55, 65, 55, 65, 300};
    LVCOLUMNW col;
    int i;

    /* 資料夾 */
    add_label(wnd, L"學生作業資料夾：", 12, 15, 110, 20);
    app.students = make_control(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, WS_EX_CLIENTEDGE, 124,
                                12, 562, 24, ID_STUDENTS);
    app.browse_students = add_button(wnd, L"選擇…", 694, 11, 88, 26, ID_BROWSE_STUDENTS);
    app.roster = add_button(wnd, L"學生名單…", 794, 11, 88, 26, ID_ROSTER);

    add_label(wnd, L"題目 (參考答案 .c)：", 12, 47, 112, 20);
    app.problem = make_control(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, WS_EX_CLIENTEDGE, 124, 44,
                               562, 24, ID_PROBLEM);
    app.browse_problem = add_button(wnd, L"選擇 .c…", 694, 43, 88, 26, ID_BROWSE_PROBLEM);
    app.edit_tests = add_button(wnd, L"編輯測資…", 794, 43, 88, 26, ID_EDIT_TESTS);

    /* 模板 (只是快速填好下面的設定)、滿分、時間 */
    add_label(wnd, L"快速套用模板：", 12, 81, 96, 20);
    app.template_combo = make_control(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                      0, 110, 77, 420, 300, ID_TEMPLATE);
    for (i = 0; i < config_template_count(); i++) {
        wchar_t *w = utf8_to_wide(config_template_name(i));
        SendMessageW(app.template_combo, CB_ADDSTRING, 0, (LPARAM)(w != NULL ? w : L""));
        free(w);
    }
    SendMessageW(app.template_combo, CB_ADDSTRING, 0, (LPARAM)L"自訂設定 (目前下方的設定不屬於任何模板)");
    SendMessageW(app.template_combo, CB_SETCURSEL, 1, 0);
    add_label(wnd, L"滿分", 552, 81, 32, 20);
    app.full_score = add_edit(wnd, 586, 78, 60, ID_FULL_SCORE);
    add_label(wnd, L"時間限制", 664, 81, 60, 20);
    app.timeout = add_edit(wnd, 726, 78, 44, ID_TIMEOUT);
    add_label(wnd, L"秒 / 題", 776, 81, 60, 20);

    /* 比對模式 */
    add_group(wnd, L"比對模式", 12, 112, 180, 180);
    app.cmp[0] = make_control(wnd, L"BUTTON", L"Strict (逐字比較)", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 0,
                              24, 134, 160, 22, ID_CMP_STRICT);
    app.cmp[1] = make_control(wnd, L"BUTTON", L"Ignore Whitespace", BS_AUTORADIOBUTTON, 0, 24, 160, 160, 22,
                              ID_CMP_WHITESPACE);
    app.cmp[2] = make_control(wnd, L"BUTTON", L"Token", BS_AUTORADIOBUTTON, 0, 24, 186, 160, 22, ID_CMP_TOKEN);
    gui_mark_muted(add_label(wnd, L"Strict：空白換行都要一樣\nIgnore WS：刪掉所有空白\nToken：空白數量不計", 24, 214, 164, 70));

    /* 比對選項 */
    add_group(wnd, L"比對選項 (標準答案與學生輸出同樣處理)", 200, 112, 300, 180);
    app.opt_trailing = add_check(wnd, L"忽略行尾空白與結尾換行", 212, 134, 280, ID_OPT_TRAILING);
    app.opt_fullwidth = add_check(wnd, L"全形半形視為相同 (： = :，１ = 1)", 212, 158, 280, ID_OPT_FULLWIDTH);
    app.opt_case = add_check(wnd, L"忽略英文大小寫", 212, 182, 130, ID_OPT_CASE);
    app.opt_blank = add_check(wnd, L"忽略空白行", 350, 182, 130, ID_OPT_BLANK);
    add_label(wnd, L"忽略含以下文字的行 (提示文字；多個用 | 分隔)：", 212, 210, 284, 20);
    app.opt_keywords = add_edit(wnd, 212, 232, 276, ID_OPT_KEYWORDS);
    gui_mark_muted(add_label(wnd, L"例如：請輸入|請依序", 212, 260, 276, 20));

    /* 評分方式：三個 radio 要連續建立，才會是同一組 */
    add_group(wnd, L"評分方式 (每題各自扣分，每題最低 0 分)", 508, 112, 404, 180);
    app.mode[0] = make_control(wnd, L"BUTTON", L"每 CHAR 扣分", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 0, 520,
                               134, 120, 22, ID_MODE_PER_CHAR);
    app.mode[1] = make_control(wnd, L"BUTTON", L"每 N CHAR 扣分", BS_AUTORADIOBUTTON, 0, 520, 162, 120, 22,
                               ID_MODE_PER_N);
    app.mode[2] = make_control(wnd, L"BUTTON", L"自訂區間", BS_AUTORADIOBUTTON, 0, 520, 190, 120, 22, ID_MODE_RANGE);

    add_label(wnd, L"每 1 CHAR 扣", 645, 136, 84, 20);
    app.per_char = add_edit(wnd, 731, 133, 52, ID_PER_CHAR);
    add_label(wnd, L"分", 789, 136, 20, 20);

    add_label(wnd, L"每", 645, 164, 18, 20);
    app.n_chars = add_edit(wnd, 665, 161, 44, ID_N_CHARS);
    add_label(wnd, L"CHAR 扣", 715, 164, 54, 20);
    app.per_n = add_edit(wnd, 771, 161, 52, ID_PER_N);
    add_label(wnd, L"分", 829, 164, 20, 20);

    app.edit_rules = add_button(wnd, L"編輯評分規則…", 645, 187, 120, 28, ID_EDIT_RULES);
    app.rule_summary = make_control(wnd, L"STATIC", L"", SS_LEFT, 0, 520, 222, 384, 60, ID_RULE_SUMMARY);

    /* 編譯失敗與執行異常 */
    app.panel_ce = add_group(wnd, L"編譯失敗與執行異常的處理", 12, 300, 900, 112);
    app.ai_fix = add_check(wnd, L"編譯失敗時交給本機 AI 做最小修正後繼續批改", 24, 322, 300, ID_AI_FIX);
    add_label(wnd, L"工具", 332, 325, 34, 20);
    app.ai_tool = make_control(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, 0,
                               366, 321, 110, 200, ID_AI_TOOL);
    for (i = 0; i < AI_TOOL_FIXED_COUNT; i++) {
        wchar_t *w = utf8_to_wide(i == 0 ? "auto (自動偵測)" : AI_TOOL_NAMES[i]);
        SendMessageW(app.ai_tool, CB_ADDSTRING, 0, (LPARAM)(w != NULL ? w : L""));
        free(w);
    }
    SendMessageW(app.ai_tool, CB_SETCURSEL, 0, 0);
    add_label(wnd, L"每修正 1 CHAR 扣", 488, 325, 104, 20);
    app.ai_per_char = add_edit(wnd, 594, 322, 44, ID_AI_PER_CHAR);
    add_label(wnd, L"分，上限", 644, 325, 58, 20);
    app.ai_max = add_edit(wnd, 704, 322, 44, ID_AI_MAX);
    add_label(wnd, L"分 (0 = 不設上限)", 754, 325, 140, 20);
    add_label(wnd, L"修正扣分最少扣滿分的", 44, 353, 130, 20);
    app.ai_min = add_edit(wnd, 176, 350, 40, ID_AI_MIN);
    add_label(wnd, L"%", 220, 353, 16, 20);
    add_label(wnd, L"AI 修改超過", 250, 353, 74, 20);
    app.ai_max_chars = add_edit(wnd, 326, 350, 44, ID_AI_MAX_CHARS);
    add_label(wnd, L"字就不採用", 376, 353, 76, 20);
    add_label(wnd, L"編譯失敗保底：滿分的", 488, 353, 132, 20);
    app.ce_floor = add_edit(wnd, 622, 350, 40, ID_CE_FLOOR);
    add_label(wnd, L"% (CE 不會是 0 分)", 668, 353, 150, 20);
    app.re_zero = add_check(wnd, L"程式當掉 (Runtime Error) 的題目直接 0 分", 24, 380, 300, ID_RE_ZERO);
    app.ai_hint = add_label(wnd, L"總扣分 = 各題輸出扣分 + 修正扣分；AI 修不好、改太多或找不到工具時給保底分。",
                            332, 383, 570, 20);
    gui_mark_muted(app.ai_hint);
    gui_mark_muted(app.rule_summary);

    /* 動作按鈕 */
    app.verify = add_button(wnd, L"驗證參考答案", 12, 424, 112, 32, ID_VERIFY);
    app.start = add_button(wnd, L"▶ 開始批改", 130, 424, 118, 32, ID_START);
    SendMessageW(app.start, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
    gui_mark_primary(app.start);
    app.export_csv = add_button(wnd, L"匯出 CSV", 254, 424, 90, 32, ID_EXPORT);
    app.open_report = add_button(wnd, L"老師版報告", 350, 424, 96, 32, ID_OPEN_REPORT);
    app.student_reports = add_button(wnd, L"學生版報告", 452, 424, 96, 32, ID_STUDENT_REPORTS);
    app.detail = add_button(wnd, L"查看詳細", 554, 424, 84, 32, ID_DETAIL);
    app.view_source = add_button(wnd, L"查看原始碼", 644, 424, 92, 32, ID_VIEW_SOURCE);
    app.progress = make_control(wnd, PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, 748, 431, 152, 18, ID_PROGRESS);

    app.list = make_control(wnd, WC_LISTVIEWW, L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP | WS_GROUP, 0,
                            12, 468, 900, 380, ID_LIST);
    ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    gui_style_listview(app.list);
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    for (i = 0; i < COL_COUNT; i++) {
        col.fmt = (i >= COL_PASSED && i <= COL_SCORE) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.cx = dpi(widths[i]);
        col.pszText = (wchar_t *)columns[i];
        ListView_InsertColumn(app.list, i, &col);
    }

    /* 狀態列 (視窗底部) */
    app.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, wnd,
                                 (HMENU)(INT_PTR)ID_STATUS, g_inst, NULL);
    SendMessageW(app.status, WM_SETFONT, (WPARAM)g_font, TRUE);
    set_status(L"請選擇學生作業資料夾與題目 (參考答案 .c)。");

    create_tooltips();
}

static void layout(void)
{
    RECT rc, sr;
    int w, h, status_h;

    GetClientRect(app.wnd, &rc);
    w = MulDiv(rc.right, 96, dpi(96));
    h = MulDiv(rc.bottom, 96, dpi(96));

    SendMessageW(app.status, WM_SIZE, 0, 0); /* 狀態列自己貼到底部 */
    GetWindowRect(app.status, &sr);
    status_h = MulDiv(sr.bottom - sr.top, 96, dpi(96));

    move_control(app.students, 124, 12, w - 124 - 208, 24);
    move_control(app.browse_students, w - 200, 11, 88, 26);
    move_control(app.roster, w - 100, 11, 88, 26);
    move_control(app.problem, 124, 44, w - 124 - 208, 24);
    move_control(app.browse_problem, w - 200, 43, 88, 26);
    move_control(app.edit_tests, w - 100, 43, 88, 26);
    app.panels[app.panel_ce].rc.right = w - 12;
    InvalidateRect(app.wnd, NULL, TRUE);
    move_control(app.ai_hint, 332, 383, w - 344, 20);
    move_control(app.progress, 748, 431, w - 748 - 12, 18);
    move_control(app.list, 12, 468, w - 24, h - 468 - status_h - 8);
}

/* 表格列顏色：全對 綠、編譯失敗 紅、AI 修正 藍、執行異常 黃 */
static LRESULT on_list_custom_draw(NMLVCUSTOMDRAW *cd)
{
    int index;
    const StudentResult *r;
    int j;

    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT)
        return CDRF_DODEFAULT;

    index = (int)cd->nmcd.lItemlParam;
    if (!is_graded(index)) { /* 還沒批改：交錯底色 */
        cd->clrText = g_theme.text;
        cd->clrTextBk = (cd->nmcd.dwItemSpec % 2) ? g_theme.row_alt : g_theme.field;
        return CDRF_NEWFONT;
    }
    r = &app.results[index];

    cd->clrText = g_theme.text;
    if (!student_ran_tests(r->status)) {
        cd->clrTextBk = g_theme.row_bad;
        return CDRF_NEWFONT;
    }
    if (r->status == STUDENT_CE_FIXED) {
        cd->clrTextBk = g_theme.row_fixed;
        return CDRF_NEWFONT;
    }
    for (j = 0; j < r->test_count; j++) {
        TestStatus s = r->tests[j].status;
        if (s == TEST_RUNTIME_ERROR || s == TEST_TIMEOUT || s == TEST_OUTPUT_LIMIT) {
            cd->clrTextBk = g_theme.row_warn;
            return CDRF_NEWFONT;
        }
    }
    if (r->passed == r->test_count) {
        cd->clrTextBk = g_theme.row_ok;
        return CDRF_NEWFONT;
    }
    cd->clrTextBk = (cd->nmcd.dwItemSpec % 2) ? g_theme.row_alt : g_theme.field;
    return CDRF_NEWFONT;
}

static LRESULT CALLBACK main_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        app.wnd = wnd;
        create_controls(wnd);
        gui_theme_window(wnd);
        return 0;

    case WM_DRAWITEM:
        if (gui_theme_drawitem((DRAWITEMSTRUCT *)lp))
            return TRUE;
        return 0;

    case WM_PAINT:
        paint_panels(wnd);
        return 0;

    case WM_SIZE:
        if (app.list != NULL)
            layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        RECT rc = {0, 0, dpi(940), dpi(668)};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        mm->ptMinTrackSize.x = rc.right - rc.left;
        mm->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BROWSE_STUDENTS:
            if (browse_folder(app.students, L"選擇學生作業資料夾 (每個子資料夾是一位學生，例如從教學平台下載解壓縮的資料夾)"))
                check_folders(0);
            break;
        case ID_BROWSE_PROBLEM:
            if (browse_reference())
                check_folders(1);
            break;
        case ID_STUDENT_REPORTS:
            on_student_reports();
            break;
        case ID_EDIT_TESTS:
            on_edit_tests();
            break;
        case ID_ROSTER:
            on_roster();
            break;
        case ID_STUDENTS:
        case ID_PROBLEM:
            if (HIWORD(wp) == EN_KILLFOCUS && !app.busy && SendMessageW((HWND)lp, EM_GETMODIFY, 0, 0)) {
                SendMessageW((HWND)lp, EM_SETMODIFY, FALSE, 0);
                check_folders(LOWORD(wp) == ID_PROBLEM);
            }
            break;
        case ID_TEMPLATE:
            if (HIWORD(wp) == CBN_SELCHANGE)
                on_template_selected();
            break;
        /* 下方設定一有變動，模板選單就重新判斷 (符合的模板或「自訂設定」) */
        case ID_MODE_PER_CHAR:
        case ID_MODE_PER_N:
        case ID_MODE_RANGE:
            update_mode_controls();
            refresh_template_match();
            break;
        case ID_CMP_STRICT:
        case ID_CMP_WHITESPACE:
        case ID_CMP_TOKEN:
        case ID_OPT_TRAILING:
        case ID_OPT_FULLWIDTH:
        case ID_OPT_CASE:
        case ID_OPT_BLANK:
            refresh_template_match();
            break;
        case ID_PER_CHAR:
        case ID_N_CHARS:
        case ID_PER_N:
        case ID_OPT_KEYWORDS:
            if (HIWORD(wp) == EN_CHANGE)
                refresh_template_match();
            break;
        case ID_AI_FIX:
            update_mode_controls(); /* CE 自動修正不屬於模板 */
            break;
        case ID_VIEW_SOURCE:
            on_view_source();
            break;
        case ID_EDIT_RULES:
            on_edit_rules();
            break;
        case ID_VERIFY:
            start_run(1);
            break;
        case ID_START:
            start_run(0);
            break;
        case ID_EXPORT:
            on_export();
            break;
        case ID_OPEN_REPORT:
            on_open_report();
            break;
        case ID_DETAIL:
            on_detail();
            break;
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        LRESULT result;
        if (gui_theme_notify(hdr, &result))
            return result;
        if (hdr->idFrom == ID_LIST) {
            switch (hdr->code) {
            case NM_DBLCLK:
                on_detail();
                break;
            case LVN_KEYDOWN:
                if (((NMLVKEYDOWN *)lp)->wVKey == VK_RETURN)
                    on_detail();
                break;
            case LVN_COLUMNCLICK:
                on_column_click(((NMLISTVIEW *)lp)->iSubItem);
                break;
            case NM_CUSTOMDRAW:
                return on_list_custom_draw((NMLVCUSTOMDRAW *)lp);
            }
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);

    case WM_APP_STATUS: {
        wchar_t *text = (wchar_t *)lp;
        if (text != NULL) {
            set_status(text);
            free(text);
        }
        return 0;
    }
    case WM_APP_STARTED:
        on_started((int)wp);
        return 0;
    case WM_APP_STUDENT_DONE:
        on_student_done((int)wp);
        return 0;
    case WM_APP_ASK_CONTINUE:
        return on_ask_continue((const char *)lp);
    case WM_APP_FINISHED:
        on_finished((int)wp, (char *)lp);
        return 0;

    case WM_CLOSE:
        if (app.busy) {
            if (MessageBoxW(wnd, L"正在批改中，確定要結束嗎？", L"C 作業批改工具", MB_YESNO | MB_ICONQUESTION) !=
                IDYES)
                return 0;
            app.closing = 1;
            InterlockedExchange(&app.cancel, 1);
            set_status(L"正在停止批改並清除暫存檔…");
            return 0; /* 等 worker 結束 (WM_APP_FINISHED) 再關視窗 */
        }
        save_last_folders();
        DestroyWindow(wnd);
        return 0;

    case WM_DESTROY:
        free_previous_results(); /* 也會刪除暫存資料夾 */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int gui_run(HINSTANCE instance, int show)
{
    INITCOMMONCONTROLSEX icc;
    RECT rc;
    MSG msg;
    int argc;
    wchar_t **argv;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);
    executor_init();
    gui_common_init(instance);

    memset(&app, 0, sizeof(app));
    app.sort_column = COL_ID;
    app.sort_ascending = 1;
    config_default(&app.cfg);
    config_apply_template(&app.cfg, 1); /* 預設用「建議」模板 */

    register_window_class(L"CGraderMain", main_proc);
    rc.left = 0;
    rc.top = 0;
    rc.right = dpi(960);
    rc.bottom = dpi(888);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    CreateWindowExW(0, L"CGraderMain", L"C 語言作業批改工具", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                    CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, instance, NULL);
    if (app.wnd == NULL)
        return 1;

    config_to_ui(&app.cfg);

    /* 命令列：Grader.exe [學生資料夾] [題目 .c]；沒給就用上次的資料夾 */
    load_last_folders();
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != NULL) {
        if (argc >= 2)
            SetWindowTextW(app.students, argv[1]);
        if (argc >= 3)
            SetWindowTextW(app.problem, argv[2]);
        LocalFree(argv);
    }
    check_folders(1);

    ShowWindow(app.wnd, show);
    UpdateWindow(app.wnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(app.wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return (int)msg.wParam;
}
