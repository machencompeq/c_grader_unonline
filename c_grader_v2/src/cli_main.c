/*
 * cli_main.c - 命令列版本 (與 GUI 共用同一組核心模組)。
 *
 * 用法：
 *   grader_cli <參考答案.c> --students <學生資料夾> [選項]
 *
 *   參考答案.c：老師的標準程式，例如 D:\hw\hw1.c
 *               測資、設定、報告放在旁邊的 D:\hw\hw1_測資\ (testcase\、grader.cfg、reports\)
 *   學生資料夾：每個子資料夾 = 一位學生 (從教學平台下載解壓縮的資料夾)
 *   (舊格式：第一個參數也可以是 題目資料夾，裡面有 reference\ 與 testcase\)
 *
 * 選項：
 *   --config <檔案>        讀取評分設定 (沒指定時，若 測資資料夾\grader.cfg 存在就自動讀取)
 *   --template <編號>      套用評分模板 (--list-templates 看編號)
 *   --csv <檔案>           匯出 CSV
 *   --no-report            不產生 HTML 比對報告 (預設會產生在 測資資料夾\reports\)
 *   --no-ai-fix            編譯失敗直接給保底分，不呼叫本機 AI 修正 (覆蓋設定檔的 ai_fix)
 *   --ai-tool <名稱|命令>  指定 AI 工具：auto / claude / codex / gemini / 自訂命令列
 *   --tests-export <檔案>  把所有測資輸出成一份「總表」文字檔後結束 (方便整批修改)
 *   --tests-import <檔案>  用總表文字檔取代所有測資後結束 (AI 腳本的輸出也可以直接匯入)
 *   --make-roster          產生 / 更新 名單.csv (資料夾名稱 -> 姓名、學號) 後結束
 *   --detail               顯示每個 testcase 的細節
 *   --verify-only          只驗證參考程式，不批改學生
 *   --keep-temp            保留暫存資料夾
 *   --write-config <檔案>  把目前設定 (含 --template) 寫成設定檔後結束
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "executor.h"
#include "exporter.h"
#include "grader.h"
#include "report.h"
#include "util.h"

static void print_usage(void)
{
    printf("用法：grader_cli <參考答案.c> --students 學生資料夾 [--config 檔案] [--template 編號]\n"
           "                 [--csv 檔案] [--no-report] [--no-ai-fix] [--ai-tool 名稱] [--detail]\n"
           "                 [--verify-only] [--keep-temp]\n"
           "       grader_cli <參考答案.c> --tests-export 總表.txt | --tests-import 總表.txt\n"
           "       grader_cli <參考答案.c> --students 學生資料夾 --make-roster\n"
           "       grader_cli --list-templates\n"
           "       grader_cli [--template 編號] --write-config 檔案\n");
}

static void print_templates(void)
{
    int i;
    printf("評分模板：\n");
    for (i = 0; i < config_template_count(); i++)
        printf("\n  [%d] %s\n      %s\n", i, config_template_name(i), config_template_description(i));
}

static void print_test_detail(const Grader *g, const StudentResult *r)
{
    int j;
    for (j = 0; j < r->test_count; j++) {
        const TestResult *t = &r->tests[j];
        char full[64], score[64], diff[32];
        format_score(full, sizeof(full), t->full);
        format_score(score, sizeof(score), t->score);
        if (t->diff < 0)
            snprintf(diff, sizeof(diff), "-");
        else
            snprintf(diff, sizeof(diff), "%ld%s", t->diff, t->approximate ? "(近似)" : "");
        printf("      %-10s %-4s diff=%-8s score=%s/%s  (%d ms)\n", g->tests.items[j].name,
               test_status_name(t->status), diff, score, full, t->elapsed_ms);
    }
    if (r->status == STUDENT_CE_FIXED) {
        char pen[64];
        format_score(pen, sizeof(pen), r->fix_penalty);
        printf("      AI 修正 (%s)：%ld CHAR，修正扣分 %s，嘗試 %d 次\n", r->fix_tool, r->fix_chars, pen,
               r->fix_attempts);
    }
}

int main(void)
{
    int argc, i;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = calloc(argc + 1, sizeof(char *));
    const char *problem = NULL, *students = NULL, *config_path = NULL, *csv_path = NULL, *write_config = NULL;
    const char *tests_export = NULL, *tests_import = NULL, *ai_tool = NULL;
    int detail = 0, verify_only = 0, keep_temp = 0, no_report = 0, make_roster = 0, template_index = -1;
    int no_ai_fix = 0;
    char auto_config[GRADER_PATH_MAX], report_dir[GRADER_PATH_MAX];
    char reference[GRADER_PATH_MAX], data_dir[GRADER_PATH_MAX];
    char err[2048] = "", msg[4096];
    GradeConfig cfg;
    Grader g;
    ReferenceReport report;
    StudentResult *results;
    StrBuf settings = {0};
    int exit_code = 0;

    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    executor_init();

    if (wargv == NULL || argv == NULL) {
        printf("無法讀取命令列參數。\n");
        return 2;
    }
    for (i = 0; i < argc; i++)
        argv[i] = wide_to_utf8(wargv[i]);
    LocalFree(wargv);

    for (i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            print_usage();
            return 2;
        }
        if (strcmp(argv[i], "--students") == 0 && i + 1 < argc)
            students = argv[++i];
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--template") == 0 && i + 1 < argc)
            template_index = atoi(argv[++i]);
        else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
            csv_path = argv[++i];
        else if (strcmp(argv[i], "--write-config") == 0 && i + 1 < argc)
            write_config = argv[++i];
        else if (strcmp(argv[i], "--ai-tool") == 0 && i + 1 < argc)
            ai_tool = argv[++i];
        else if (strcmp(argv[i], "--list-templates") == 0) {
            print_templates();
            return 0;
        } else if (strcmp(argv[i], "--detail") == 0)
            detail = 1;
        else if (strcmp(argv[i], "--verify-only") == 0)
            verify_only = 1;
        else if (strcmp(argv[i], "--keep-temp") == 0)
            keep_temp = 1;
        else if (strcmp(argv[i], "--make-roster") == 0)
            make_roster = 1;
        else if (strcmp(argv[i], "--no-ai-fix") == 0)
            no_ai_fix = 1;
        else if (strcmp(argv[i], "--tests-export") == 0 && i + 1 < argc)
            tests_export = argv[++i];
        else if (strcmp(argv[i], "--tests-import") == 0 && i + 1 < argc)
            tests_import = argv[++i];
        else if (strcmp(argv[i], "--no-report") == 0)
            no_report = 1;
        else if (argv[i][0] != '-' && problem == NULL)
            problem = argv[i];
        else {
            print_usage();
            return 2;
        }
    }
    if (template_index >= config_template_count()) {
        printf("沒有編號 %d 的模板。\n", template_index);
        print_templates();
        return 2;
    }

    config_default(&cfg);
    if (write_config != NULL) {
        if (template_index >= 0)
            config_apply_template(&cfg, template_index);
        if (!config_save(write_config, &cfg)) {
            printf("無法寫入 %s\n", write_config);
            return 1;
        }
        printf("已寫入設定檔：%s\n", write_config);
        return 0;
    }
    if (problem == NULL) {
        print_usage();
        return 2;
    }
    grader_problem_paths(problem, reference, data_dir, sizeof(data_dir));

    /* 測資總表：匯出 / 匯入 */
    if (tests_export != NULL) {
        char *text = testcase_export_text(data_dir);
        if (!write_file(tests_export, text, strlen(text))) {
            printf("無法寫入 %s\n", tests_export);
            free(text);
            return 1;
        }
        free(text);
        printf("已匯出測資總表：%s\n", tests_export);
        return 0;
    }
    if (tests_import != NULL) {
        size_t len = 0;
        char *text = read_file(tests_import, &len);
        int n;
        if (text == NULL) {
            printf("無法讀取 %s\n", tests_import);
            return 1;
        }
        text = text_to_utf8(text, &len);
        n = testcase_import_text(data_dir, text, err, sizeof(err));
        free(text);
        if (n < 0) {
            printf("總表格式錯誤：%s\n", err);
            return 1;
        }
        printf("已匯入 %d 組測資到 %s\\testcase\n", n, data_dir);
        return 0;
    }

    /* 讀取設定：設定檔 -> 模板 (模板會覆蓋比對與扣分方式) -> 命令列覆蓋 */
    if (config_path == NULL) {
        path_join(auto_config, sizeof(auto_config), data_dir, "grader.cfg");
        if (file_exists(auto_config))
            config_path = auto_config;
    }
    if (config_path != NULL) {
        if (!config_load(config_path, &cfg, err, sizeof(err))) {
            printf("設定檔錯誤：%s\n", err);
            return 1;
        }
        printf("設定檔：%s\n", config_path);
    }
    if (template_index >= 0) {
        config_apply_template(&cfg, template_index);
        printf("模板：%s\n", config_template_name(template_index));
    }
    if (keep_temp)
        cfg.keep_temp = 1;
    if (no_ai_fix)
        cfg.ai_fix = 0;
    if (ai_tool != NULL)
        snprintf(cfg.ai_fix_tool, sizeof(cfg.ai_fix_tool), "%s", ai_tool);

    switch (score_validate(&cfg.rules, msg, sizeof(msg))) {
    case RULES_ERROR:
        printf("評分規則有錯誤，無法批改：\n%s", msg);
        return 1;
    case RULES_WARNING:
        printf("評分規則提醒：\n%s\n", msg);
        break;
    case RULES_OK:
        break;
    }
    describe_settings(&cfg, &settings);
    printf("%s\n\n", settings.data);
    sb_free(&settings);

    if (make_roster) {
        /* 名單只需要學生資料夾，不需要參考答案與測資 */
        char roster_path[GRADER_PATH_MAX], resolved[GRADER_PATH_MAX];
        StringList names = {0};
        int added;
        grader_resolve_students_dir(students != NULL ? students : data_dir, resolved, sizeof(resolved));
        grader_list_students(resolved, &names);
        roster_default_path(roster_path, sizeof(roster_path));
        added = roster_sync(roster_path, &names);
        string_list_free(&names);
        if (added < 0)
            printf("無法寫入 %s\n", roster_path);
        else
            printf("已更新學生名單：%s (新增 %d 位)。請填入「姓名」欄後存檔。\n", roster_path, added);
        return added < 0;
    }

    if (!grader_open(&g, problem, students, &cfg, err, sizeof(err))) {
        printf("錯誤：%s\n", err);
        return 1;
    }
    printf("參考答案：%s\n測資資料夾：%s\n學生資料夾：%s\n找到 %d 組測資、%d 位學生\n\n", g.reference, g.problem_dir,
           g.students_dir, g.tests.count, g.students.count);

    /* 參考程式 */
    printf("[1] 編譯並驗證參考程式...\n");
    if (!grader_prepare_reference(&g, &report, err, sizeof(err))) {
        printf("錯誤：%s\n", err);
        if (report.compile_log != NULL && report.compile_log[0] != '\0')
            printf("---- compiler error ----\n%s\n------------------------\n", report.compile_log);
        reference_report_free(&report);
        grader_close(&g);
        return 1;
    }
    for (i = 0; i < report.count; i++) {
        const ReferenceCheck *c = &report.checks[i];
        if (!c->has_out)
            printf("    %-10s 沒有 .out，使用參考程式輸出作為標準答案\n", c->name);
        else if (c->run_status != RUN_OK)
            printf("    %-10s ⚠ 參考程式執行失敗：%s\n", c->name, run_status_name(c->run_status));
        else if (c->diff != 0)
            printf("    %-10s ⚠ 參考程式輸出與 .out 不一致 (差異 %ld CHAR)\n", c->name, c->diff);
        else
            printf("    %-10s OK\n", c->name);
    }
    if (report.mismatches > 0 || report.failures > 0)
        printf("⚠ 參考程式輸出與 testcase 標準答案不一致 (批改仍以 .out 為準)\n");
    printf("\n");
    reference_report_free(&report);

    if (verify_only) {
        grader_close(&g);
        return 0;
    }

    /* 學生 */
    printf("[2] 批改學生程式...\n\n");
    printf("%-28s %-14s %-7s %-8s %-8s %-8s %s\n", "學號", "狀態", "通過", "差異數", "扣分", "成績",
           "各題");
    results = calloc(g.students.count > 0 ? g.students.count : 1, sizeof(StudentResult));
    if (results == NULL) {
        printf("記憶體不足\n");
        grader_close(&g);
        return 1;
    }
    for (i = 0; i < g.students.count; i++) {
        StudentResult *r = &results[i];
        char score[64], deduction[64], diff[32], passed[32], label[400], status[48];
        StrBuf per_test = {0};
        int j;

        grader_grade_student(&g, i, r);
        if (r->name[0] != '\0')
            snprintf(label, sizeof(label), "%s %s", r->name, r->id);
        else
            snprintf(label, sizeof(label), "%s", r->id);

        format_score(score, sizeof(score), r->score);
        format_score(deduction, sizeof(deduction), r->total_deduction);
        if (r->total_diff < 0)
            snprintf(diff, sizeof(diff), "-");
        else
            snprintf(diff, sizeof(diff), "%ld", r->total_diff);
        snprintf(passed, sizeof(passed), "%d/%d", r->passed, r->test_count);
        for (j = 0; j < r->test_count; j++)
            sb_appendf(&per_test, "%s%s", j > 0 ? " " : "", test_status_name(r->tests[j].status));
        if (r->status == STUDENT_CE_FIXED)
            snprintf(status, sizeof(status), "CE→修正(%ld)", r->fix_chars);
        else
            snprintf(status, sizeof(status), "%s", student_status_name(r->status));

        if (student_ran_tests(r->status))
            printf("%-26s %-12s %-6s %-6s %-6s %-6s %s\n", label, status, passed, diff, deduction, score,
                   per_test.data);
        else
            printf("%-26s %-12s %-6s %-6s %-6s %-6s\n", label, status, passed, "-", "-", score);
        if (r->note[0] != '\0')
            printf("      ※ %s\n", r->note);
        sb_free(&per_test);

        if (detail) {
            if (r->status == STUDENT_COMPILE_ERROR)
                printf("---- compiler error ----\n%s------------------------\n", r->compile_log);
            else if (student_ran_tests(r->status))
                print_test_detail(&g, r);
        }
        fflush(stdout);
    }

    if (!no_report) {
        for (i = 0; i < g.students.count; i++)
            if (!report_write_both(&g, &results[i], err, sizeof(err)))
                printf("%s\n", err);
        if (report_write_teacher_index(&g, results, g.students.count, err, sizeof(err))) {
            report_default_dir(&g, REPORT_TEACHER, report_dir, sizeof(report_dir));
            printf("\n老師版報告：%s\\index.html (全班總表，含隱藏測資)\n", report_dir);
            report_default_dir(&g, REPORT_STUDENT, report_dir, sizeof(report_dir));
            printf("學生版報告：%s (每人一份，可直接發給學生)\n", report_dir);
        } else {
            printf("\n%s\n", err);
        }
    }

    if (csv_path != NULL) {
        if (export_csv(csv_path, &g, results, g.students.count, err, sizeof(err)))
            printf("已匯出 CSV：%s\n", csv_path);
        else {
            printf("%s\n", err);
            exit_code = 1;
        }
    }

    for (i = 0; i < g.students.count; i++)
        student_result_free(&results[i]);
    free(results);
    grader_close(&g);
    for (i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    return exit_code;
}
