/*
 * gui_tests.c - 「編輯測資」視窗：所有測資放在同一份「總表」，一眼看完、直接整批修改。
 *
 *   ┌測資總表 (可直接修改)──────────────┐┌參考答案輸出預覽────────────┐
 *   │===== test01 | 可見 | 一般情況 =====││===== test01 | 可見 … =====│
 *   │資訊管理學系 一年級 甲班 ...        ││系所: 資訊管理學系 ...      │
 *   │===== test02 | 隱藏 | 英文 =====    ││                            │
 *   └────────────────────────────────────┘└────────────────────────────┘
 *   共 2 組：可見 1、隱藏 1
 *   [新增一組] [AI 產生測資] 組數[8] 要求[........] [預覽參考答案輸出]      [儲存] [取消]
 *
 * 格式說明見 testcase.h。儲存時依順序重新編號 test01、test02…
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "grader.h"
#include "gui.h"
#include "testcase.h"
#include "util.h"

enum {
    ID_T_EDITOR = 400,
    ID_T_PREVIEW,
    ID_T_ADD,
    ID_T_AI,
    ID_T_COUNT,
    ID_T_NOTE,
    ID_T_RUN_PREVIEW
};

static struct {
    HWND wnd, help, editor_label, editor, preview_label, preview, summary;
    HWND add, ai, count_label, count, note_label, note, run_preview, save, cancel;
    char reference[GRADER_PATH_MAX];
    char data_dir[GRADER_PATH_MAX];
    GradeConfig cfg;
    volatile int done;
} T;

/* 編輯框內容 -> UTF-8 (\r\n 換回 \n) */
static char *editor_text(HWND edit)
{
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
    char *s, *p, *q;

    if (w == NULL)
        return _strdup("");
    GetWindowTextW(edit, w, n + 1);
    s = wide_to_utf8(w);
    free(w);
    if (s == NULL)
        return _strdup("");
    for (p = q = s; *p != '\0'; p++)
        if (*p != '\r')
            *q++ = *p;
    *q = '\0';
    return s;
}

static void set_editor_utf8(HWND edit, const char *text)
{
    wchar_t *w = make_display_text(text, strlen(text), 0);
    SetWindowTextW(edit, w);
    free(w);
}

/* 數一數總表裡有幾組、幾組隱藏 (標題行判斷規則與 testcase.c 相同) */
static void update_summary(void)
{
    char *text = editor_text(T.editor), *line, msg[160];
    int total = 0, hidden = 0;

    for (line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, "===== ", 6) == 0 && strstr(line + 5, " =====") != NULL) {
            total++;
            if (strstr(line, "隱藏") != NULL || strstr(line, "hidden") != NULL)
                hidden++;
        }
    }
    snprintf(msg, sizeof(msg), "共 %d 組：可見 %d、隱藏 %d　(隱藏測資照常計分；學生版報告不顯示它的輸入與答案)", total,
             total - hidden, hidden);
    set_text_utf8(T.summary, msg);
    free(text);
}

static void append_to_editor(const char *text)
{
    wchar_t *w = make_display_text(text, strlen(text), 0);
    int n = GetWindowTextLengthW(T.editor);

    SendMessageW(T.editor, EM_SETSEL, n, n);
    SendMessageW(T.editor, EM_REPLACESEL, TRUE, (LPARAM)w);
    SendMessageW(T.editor, EM_SCROLLCARET, 0, 0);
    free(w);
}

static void on_add(void)
{
    char *text = editor_text(T.editor);
    size_t len = strlen(text);
    StrBuf block = {0};

    if (len > 0 && text[len - 1] != '\n')
        sb_append(&block, "\n");
    sb_append(&block, "===== test | 可見 | 說明 =====\n(在這裡寫輸入)\n");
    append_to_editor(block.data);
    sb_free(&block);
    free(text);
    SetFocus(T.editor);
    update_summary();
}

/* ---------------- 預覽：用參考答案實際執行目前總表裡的每一組輸入 ---------------- */

static void on_run_preview(void)
{
    char *text = editor_text(T.editor), tmp[GRADER_PATH_MAX], err[1024];
    GradeConfig cfg = T.cfg;
    ReferenceReport report;
    StrBuf out = {0};
    Grader g;
    HCURSOR old;
    int i, n;

    /* 先把總表寫到暫存資料夾 (不影響正式測資)，再用批改流程同一套方式執行參考答案 */
    if (!make_run_temp_dir(tmp, sizeof(tmp))) {
        free(text);
        return;
    }
    n = testcase_import_text(tmp, text, err, sizeof(err));
    free(text);
    if (n < 0) {
        wchar_t *w = utf8_to_wide(err);
        MessageBoxW(T.wnd, w != NULL ? w : L"格式錯誤", L"編輯測資", MB_ICONWARNING);
        free(w);
        remove_dir_recursive(tmp);
        return;
    }

    old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    set_text_utf8(T.summary, "編譯並執行參考答案中…");
    if (cfg.timeout_ms > 10000)
        cfg.timeout_ms = 10000; /* 預覽不用等到 60 秒 */

    if (!grader_open_ex(&g, T.reference, tmp, NULL, &cfg, err, sizeof(err))) {
        sb_appendf(&out, "無法預覽：%s\n", err);
    } else {
        if (!grader_prepare_reference(&g, &report, err, sizeof(err))) {
            sb_appendf(&out, "%s\n", err);
            if (report.compile_log != NULL)
                sb_appendf(&out, "\n%s", report.compile_log);
        } else {
            for (i = 0; i < g.tests.count; i++) {
                const TestCase *tc = &g.tests.items[i];
                const ReferenceCheck *c = &report.checks[i];
                sb_appendf(&out, "===== %s | %s | %s =====\n", tc->name, tc->hidden ? "隱藏" : "可見", tc->desc);
                if (c->run_status != RUN_OK)
                    sb_appendf(&out, "⚠ 參考答案執行失敗：%s\n", run_status_name(c->run_status));
                if (tc->has_out) {
                    sb_append(&out, c->diff > 0 ? "⚠ 你指定的標準答案與參考答案輸出不同，批改以指定答案為準：\n"
                                                : "(使用你指定的標準答案)\n");
                }
                if (g.expected[i] != NULL) {
                    char *copy = malloc(g.expected_len[i] + 1);
                    if (copy != NULL) {
                        memcpy(copy, g.expected[i], g.expected_len[i]);
                        copy[g.expected_len[i]] = '\0';
                        sb_append(&out, copy);
                        if (g.expected_len[i] > 0 && copy[g.expected_len[i] - 1] != '\n')
                            sb_append(&out, "\n");
                        free(copy);
                    }
                }
            }
        }
        reference_report_free(&report);
        grader_close(&g);
    }
    remove_dir_recursive(tmp);

    set_editor_utf8(T.preview, out.data != NULL ? out.data : "");
    sb_free(&out);
    SetCursor(old);
    update_summary();
}

/* ---------------- AI 產生測資 (呼叫 tools\ai_testcases.ps1) ---------------- */

static void on_ai(void)
{
    char script[GRADER_PATH_MAX], out_file[GRADER_PATH_MAX], tmp[GRADER_PATH_MAX];
    char *note, *p;
    double count;
    StrBuf cmd = {0};
    int exit_code;

    if (!file_exists(T.reference)) {
        MessageBoxW(T.wnd, L"AI 產生測資需要題目是一個 .c 檔。", L"AI 產生測資", MB_ICONINFORMATION);
        return;
    }
    if (!tool_script_path("ai_testcases.ps1", script, sizeof(script))) {
        MessageBoxW(T.wnd, L"找不到 tools\\ai_testcases.ps1 (應該放在 Grader.exe 旁邊的 tools 資料夾)。", L"AI 產生測資",
                    MB_ICONERROR);
        return;
    }
    if (!read_number(T.count, &count) || count < 1 || count > 50)
        count = 8;

    if (!make_run_temp_dir(tmp, sizeof(tmp)))
        return;
    path_join(out_file, sizeof(out_file), tmp, "ai.txt");

    note = editor_text(T.note);
    for (p = note; *p != '\0'; p++)
        if (*p == '"' || *p == '\n')
            *p = ' '; /* 命令列參數裡不能有雙引號 */

    sb_appendf(&cmd,
               "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -Reference \"%s\" -Count %d -Out \"%s\" -Pause",
               script, T.reference, (int)count, out_file);
    if (note[0] != '\0')
        sb_appendf(&cmd, " -Note \"%s\"", note);
    free(note);

    /* 開一個看得到的主控台視窗，老師可以看到 AI 的進度 */
    set_text_utf8(T.summary, "AI 產生測資中… (請看另外開啟的視窗)");
    exit_code = run_visible_command(T.wnd, cmd.data);
    sb_free(&cmd);
    if (exit_code < 0) {
        MessageBoxW(T.wnd, L"無法啟動 PowerShell。", L"AI 產生測資", MB_ICONERROR);
        remove_dir_recursive(tmp);
        return;
    }

    if (exit_code == 0 && file_exists(out_file)) {
        size_t len = 0;
        char *result = read_file(out_file, &len);
        if (result != NULL) {
            int answer = MessageBoxW(T.wnd,
                                     L"AI 已產生測資。\n\n「是」= 加在現有測資後面\n「否」= 取代現有測資\n「取消」= 不要使用",
                                     L"AI 產生測資", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDYES) {
                char *current = editor_text(T.editor);
                size_t cur_len = strlen(current);
                if (cur_len > 0 && current[cur_len - 1] != '\n')
                    append_to_editor("\n");
                append_to_editor(result);
                free(current);
            } else if (answer == IDNO) {
                set_editor_utf8(T.editor, result);
                SendMessageW(T.editor, EM_SETMODIFY, TRUE, 0);
            }
            free(result);
        }
    } else {
        MessageBoxW(T.wnd, L"AI 沒有成功產生測資，請看 PowerShell 視窗裡的訊息。", L"AI 產生測資", MB_ICONWARNING);
    }
    remove_dir_recursive(tmp);
    update_summary();
}

/* ---------------- 儲存 / 關閉 ---------------- */

static int save(void)
{
    char *text = editor_text(T.editor), err[1024], msg[256];
    int n = testcase_import_text(T.data_dir, text, err, sizeof(err));

    free(text);
    if (n < 0) {
        wchar_t *w = utf8_to_wide(err);
        MessageBoxW(T.wnd, w != NULL ? w : L"格式錯誤", L"儲存測資", MB_ICONWARNING);
        free(w);
        return 0;
    }
    /* 重新讀回來，讓畫面顯示重新編號後的內容 */
    text = testcase_export_text(T.data_dir);
    set_editor_utf8(T.editor, text);
    free(text);
    SendMessageW(T.editor, EM_SETMODIFY, FALSE, 0);
    snprintf(msg, sizeof(msg), "已儲存 %d 組測資。", n);
    set_text_utf8(T.summary, msg);
    return 1;
}

static void close_dialog(void)
{
    if (SendMessageW(T.editor, EM_GETMODIFY, 0, 0)) {
        int answer = MessageBoxW(T.wnd, L"測資有修改還沒儲存，要儲存嗎？", L"編輯測資", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL)
            return;
        if (answer == IDYES && !save())
            return;
    }
    T.done = 1;
}

static void layout(void)
{
    RECT rc;
    int w, h, left_w, box_h;

    GetClientRect(T.wnd, &rc);
    w = MulDiv(rc.right, 96, dpi(96));
    h = MulDiv(rc.bottom, 96, dpi(96));
    left_w = (w - 30) * 55 / 100;
    box_h = h - 170;

    move_control(T.help, 10, 8, w - 20, 40);
    move_control(T.editor_label, 10, 52, left_w, 20);
    move_control(T.editor, 10, 74, left_w, box_h);
    move_control(T.preview_label, 20 + left_w, 52, w - 30 - left_w, 20);
    move_control(T.preview, 20 + left_w, 74, w - 30 - left_w, box_h);
    move_control(T.summary, 10, 80 + box_h, w - 20, 20);
    move_control(T.add, 10, h - 44, 84, 30);
    move_control(T.ai, 100, h - 44, 104, 30);
    move_control(T.count_label, 212, h - 38, 34, 20);
    move_control(T.count, 248, h - 41, 36, 24);
    move_control(T.note_label, 292, h - 38, 34, 20);
    move_control(T.note, 328, h - 41, w - 328 - 348, 24);
    move_control(T.run_preview, w - 330, h - 44, 130, 30);
    move_control(T.save, w - 192, h - 44, 86, 30);
    move_control(T.cancel, w - 98, h - 44, 86, 30);
}

static LRESULT CALLBACK tests_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (T.cancel != NULL)
            layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = dpi(820);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = dpi(420);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_T_EDITOR:
            if (HIWORD(wp) == EN_CHANGE)
                update_summary();
            break;
        case ID_T_ADD:
            on_add();
            break;
        case ID_T_AI:
            on_ai();
            break;
        case ID_T_RUN_PREVIEW:
            on_run_preview();
            break;
        case IDOK:
            if (save())
                T.done = 1;
            break;
        case IDCANCEL:
            close_dialog();
            break;
        }
        return 0;
    case WM_NOTIFY: {
        LRESULT result;
        if (gui_theme_notify((NMHDR *)lp, &result))
            return result;
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return gui_theme_ctlcolor(msg, (HDC)wp, (HWND)lp);
    case WM_CLOSE:
        close_dialog();
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void tests_dialog(HWND owner, const char *reference, const char *data_dir, const GradeConfig *cfg)
{
    static int registered = 0;
    DWORD box = WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_TABSTOP;
    char *text;
    HWND wnd;

    if (!registered) {
        register_window_class(L"CGraderTests", tests_proc);
        registered = 1;
    }
    memset(&T, 0, sizeof(T));
    snprintf(T.reference, sizeof(T.reference), "%s", reference);
    snprintf(T.data_dir, sizeof(T.data_dir), "%s", data_dir);
    T.cfg = *cfg;

    wnd = create_dialog_window(L"CGraderTests", L"編輯測資 (總表)", owner, 1080, 680, 1);
    T.wnd = wnd;

    T.help = make_control(wnd, L"STATIC",
                          L"每組測資以一行「===== 名稱 | 可見 或 隱藏 | 說明 =====」開頭，下面寫輸入 (學生程式從鍵盤讀到的內容)。"
                          L"要自己指定標準答案時，在輸入下面加一行「----- 標準答案 -----」再寫答案；沒寫就用參考答案的輸出。"
                          L"可以一次貼上很多組，儲存時會依順序重新編號。",
                          SS_LEFT, 0, 10, 8, 1060, 40, -1);
    T.editor_label = make_control(wnd, L"STATIC", L"測資總表 (可直接修改)", SS_LEFT, 0, 10, 52, 580, 20, -1);
    SendMessageW(T.editor_label, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
    T.editor = make_control(wnd, L"EDIT", L"", box | ES_WANTRETURN, WS_EX_CLIENTEDGE, 10, 74, 580, 510, ID_T_EDITOR);
    T.preview_label = make_control(wnd, L"STATIC", L"參考答案輸出預覽 (按下方「預覽參考答案輸出」更新)", SS_LEFT, 0,
                                   600, 52, 470, 20, -1);
    SendMessageW(T.preview_label, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
    T.preview = make_control(wnd, L"EDIT", L"", box | ES_READONLY, WS_EX_CLIENTEDGE, 600, 74, 470, 510, ID_T_PREVIEW);
    SendMessageW(T.editor, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    SendMessageW(T.preview, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    SendMessageW(T.editor, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(T.preview, EM_SETLIMITTEXT, 0, 0);
    T.summary = make_control(wnd, L"STATIC", L"", SS_LEFT, 0, 10, 590, 1060, 20, -1);

    T.add = make_control(wnd, L"BUTTON", L"新增一組", BS_PUSHBUTTON | WS_TABSTOP, 0, 10, 636, 84, 30, ID_T_ADD);
    T.ai = make_control(wnd, L"BUTTON", L"AI 產生測資…", BS_PUSHBUTTON | WS_TABSTOP, 0, 100, 636, 104, 30, ID_T_AI);
    T.count_label = make_control(wnd, L"STATIC", L"組數", SS_LEFT, 0, 212, 642, 34, 20, -1);
    T.count = make_control(wnd, L"EDIT", L"8", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 248, 639, 36, 24,
                           ID_T_COUNT);
    T.note_label = make_control(wnd, L"STATIC", L"要求", SS_LEFT, 0, 292, 642, 34, 20, -1);
    T.note = make_control(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 328, 639, 400, 24,
                          ID_T_NOTE);
    T.run_preview = make_control(wnd, L"BUTTON", L"預覽參考答案輸出", BS_PUSHBUTTON | WS_TABSTOP, 0, 750, 636, 130,
                                 30, ID_T_RUN_PREVIEW);
    T.save = make_control(wnd, L"BUTTON", L"儲存", BS_PUSHBUTTON | WS_TABSTOP, 0, 888, 636, 86, 30, IDOK);
    gui_mark_primary(T.save);
    T.cancel = make_control(wnd, L"BUTTON", L"關閉", BS_PUSHBUTTON | WS_TABSTOP, 0, 982, 636, 86, 30, IDCANCEL);

    text = testcase_export_text(data_dir);
    set_editor_utf8(T.editor, text);
    free(text);
    SendMessageW(T.editor, EM_SETMODIFY, FALSE, 0);
    update_summary();

    layout();
    SetFocus(T.editor);
    run_modal(wnd, owner, &T.done);
}
