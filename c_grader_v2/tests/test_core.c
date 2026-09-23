/*
 * test_core.c - 核心模組的自動測試 (comparator / scorer / config / testcase / roster / ai_fix / exporter)。
 * 執行：mingw32-make test
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ai_fix.h"
#include "../src/comparator.h"
#include "../src/config.h"
#include "../src/exporter.h"
#include "../src/grader.h"
#include "../src/roster.h"
#include "../src/scorer.h"
#include "../src/testcase.h"
#include "../src/util.h"

static int failures = 0;
static int checks = 0;

#define CHECK_LONG(expr, expected)                                                        \
    do {                                                                                  \
        long got_ = (long)(expr);                                                         \
        checks++;                                                                         \
        if (got_ != (long)(expected)) {                                                   \
            printf("FAIL %s:%d  %s = %ld, expected %ld\n", __FILE__, __LINE__, #expr, got_, \
                   (long)(expected));                                                     \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_DOUBLE(expr, expected)                                                      \
    do {                                                                                  \
        double got_ = (expr);                                                             \
        checks++;                                                                         \
        if (fabs(got_ - (expected)) > 1e-9) {                                             \
            printf("FAIL %s:%d  %s = %g, expected %g\n", __FILE__, __LINE__, #expr, got_,  \
                   (double)(expected));                                                   \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_STR(expr, expected)                                                         \
    do {                                                                                  \
        const char *got_ = (expr);                                                        \
        checks++;                                                                         \
        if (got_ == NULL || strcmp(got_, (expected)) != 0) {                              \
            printf("FAIL %s:%d  %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #expr, \
                   got_ != NULL ? got_ : "(null)", (expected));                           \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static long diff_of(const char *expected, const char *actual, CompareMode mode)
{
    CompareOptions o;
    int approx;
    compare_options_default(&o);
    o.mode = mode;
    return compare_outputs(expected, strlen(expected), actual, strlen(actual), &o, &approx);
}

static long diff_opt(const char *expected, const char *actual, const CompareOptions *o)
{
    int approx;
    return compare_outputs(expected, strlen(expected), actual, strlen(actual), o, &approx);
}

static void temp_path(char *out, size_t size, const char *name)
{
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(out, size, "%s%s", tmp, name);
}

static void test_compare_options(void)
{
    CompareOptions o;

    /* 行尾空白、結尾換行 */
    compare_options_default(&o);
    o.ignore_trailing_space = 1;
    CHECK_LONG(diff_opt("abc\ndef\n", "abc   \ndef", &o), 0);
    CHECK_LONG(diff_opt("abc\ndef\n", "abc\ndef\n\n\n", &o), 0);
    CHECK_LONG(diff_opt("abc\ndef\n", "abc \nde f\n", &o), 1); /* 行中間的空白仍算 */
    CHECK_LONG(diff_opt("a b\n", " a b\n", &o), 1);           /* 行首空白仍算 */
    CHECK_LONG(diff_opt("abc\ndef", "abc\xE3\x80\x80\ndef", &o), 0); /* 行尾全形空白也不計 */
    CHECK_LONG(diff_opt("abc\ndef", "abc\t\t\ndef", &o), 0);
    CHECK_LONG(diff_opt("abc\ndef", "abc\xE3\x80\x80\ndef", &o), 0);

    /* 全形半形 */
    compare_options_default(&o);
    o.fullwidth_as_halfwidth = 1;
    CHECK_LONG(diff_opt("系所: 資工", "系所： 資工", &o), 0);
    CHECK_LONG(diff_opt("ABC 123", "Ａ＂ＢＣ　１２３", &o), 1); /* 多一個全形引號 */
    CHECK_LONG(diff_opt("ABC 123", "ＡＢＣ　１２３", &o), 0);
    CHECK_LONG(diff_opt("系所: 資工", "系所：資工", &o), 1);  /* 少一個空白仍算 */

    /* 大小寫 */
    compare_options_default(&o);
    o.ignore_case = 1;
    CHECK_LONG(diff_opt("Hello World", "hello WORLD", &o), 0);
    CHECK_LONG(diff_opt("系所", "系所", &o), 0);

    /* 空白行 */
    compare_options_default(&o);
    o.ignore_blank_lines = 1;
    CHECK_LONG(diff_opt("a\nb\n", "a\n\n   \nb\n", &o), 0);

    /* 忽略含關鍵字的行 (提示文字) */
    compare_options_default(&o);
    snprintf(o.ignore_lines_with, sizeof(o.ignore_lines_with), "請輸入 | 請依序");
    CHECK_LONG(diff_opt("請依序輸入資料:\n姓名: 王\n", "請輸入你的資料：\n姓名: 王\n", &o), 0);
    CHECK_LONG(diff_opt("請依序輸入資料:\n姓名: 王\n", "姓名: 李\n", &o), 1);
    CHECK_LONG(diff_opt("姓名: 王\n", "請輸入姓名:\n姓名: 王\n", &o), 0); /* 學生多印的提示行被忽略 */

    /* 選項 + 模式一起 */
    compare_options_default(&o);
    o.mode = COMPARE_TOKEN;
    o.fullwidth_as_halfwidth = 1;
    CHECK_LONG(diff_opt("1 2 3", "１　２　　３\n", &o), 0);

    /* 不合法的 UTF-8 (學生程式印出的亂碼)：每個 byte 各算 1 CHAR，不會讓比對出錯 */
    CHECK_LONG(diff_of("abc", "a\xFF\xFE" "bc", COMPARE_STRICT), 2);
}

/* 長輸出：n x m 超過完整動態規劃的上限時，帶狀演算法仍要給精確值 */
static void test_long_outputs(void)
{
    const size_t n = 9000;
    char *a = malloc(n + 2), *b = malloc(n + 3);
    CompareOptions o;
    size_t i, k = 0;
    int approx = 0;
    long d;

    compare_options_default(&o);
    for (i = 0; i < n; i++)
        a[i] = (char)('a' + (i * 7) % 23);
    a[n] = '\0';

    /* b = a，但第 10 個字替換、第 4000 個字前多插一個字、倒數第 10 個字替換 -> 距離 3 */
    for (i = 0; i < n; i++) {
        if (i == 4000)
            b[k++] = '#';
        b[k++] = i == 10 || i == n - 10 ? '@' : a[i];
    }
    b[k] = '\0';
    d = compare_outputs(a, n, b, k, &o, &approx);
    CHECK_LONG(d, 3);
    CHECK_LONG(approx, 0);

    /* 只在開頭多一個字：舊的「逐位置比較」會算成幾千，正確答案是 1 */
    memmove(b + 1, a, n + 1);
    b[0] = '#';
    b[n / 2] = '@'; /* 中間再替換一個，讓前後綴去不掉 */
    d = compare_outputs(a, n, b, n + 1, &o, &approx);
    CHECK_LONG(d, 2);
    CHECK_LONG(approx, 0);
    free(a);
    free(b);

    /* 超長且完全不同 (12 萬 x 12 萬 = 144 億格)：Myers 位元平行仍給精確值，不必近似 */
    {
        const size_t big = 120000;
        char *x = malloc(big + 1), *y = malloc(big + 1);
        memset(x, 'a', big);
        memset(y, 'b', big);
        x[big] = y[big] = '\0';
        approx = 0;
        d = compare_outputs(x, big, y, big, &o, &approx);
        CHECK_LONG(d, (long)big);
        CHECK_LONG(approx, 0);
        free(x);
        free(y);
    }
}

/* 教科書版動態規劃 (以字元陣列)，用來驗證 Myers 位元平行 / 帶狀演算法 */
static long naive_distance(const uint32_t *a, size_t n, const uint32_t *b, size_t m)
{
    long *prev = malloc((m + 1) * sizeof(long)), *cur = malloc((m + 1) * sizeof(long)), result;
    size_t i, j;
    for (j = 0; j <= m; j++)
        prev[j] = (long)j;
    for (i = 1; i <= n; i++) {
        cur[0] = (long)i;
        for (j = 1; j <= m; j++) {
            long best = prev[j - 1] + (a[i - 1] != b[j - 1]);
            if (prev[j] + 1 < best)
                best = prev[j] + 1;
            if (cur[j - 1] + 1 < best)
                best = cur[j - 1] + 1;
            cur[j] = best;
        }
        memcpy(prev, cur, (m + 1) * sizeof(long));
    }
    result = prev[m];
    free(prev);
    free(cur);
    return result;
}

static unsigned rng_state = 20260923u;
static unsigned rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* 隨機字串 (英文 + 中文) 與教科書版比對：長度跨越 1 個到多個 64 位元字組 */
static void test_myers_random(void)
{
    static const char *alphabet[] = {"a", "b", "c", "d", "系", "所", "\xE8\xB3\x87", " "};
    int round, mismatches = 0;

    for (round = 0; round < 600; round++) {
        size_t max_len = round < 200 ? 24 : round < 400 ? 200 : 700, la = rng() % (max_len + 1), lb = rng() % (max_len + 1);
        int letters = 2 + (int)(rng() % 7), i;
        StrBuf sa = {0}, sb = {0};
        uint32_t *ca, *cb;
        size_t na, nb;
        long got, want;
        int approx = 0;
        CompareOptions o;

        for (i = 0; i < (int)la; i++)
            sb_append(&sa, alphabet[rng() % letters]);
        for (i = 0; i < (int)lb; i++)
            sb_append(&sb, alphabet[rng() % letters]);
        ca = malloc((sa.len + 1) * sizeof(uint32_t));
        cb = malloc((sb.len + 1) * sizeof(uint32_t));
        na = compare_decode_utf8((const unsigned char *)(sa.data != NULL ? sa.data : ""), sa.len, ca);
        nb = compare_decode_utf8((const unsigned char *)(sb.data != NULL ? sb.data : ""), sb.len, cb);
        want = naive_distance(ca, na, cb, nb);
        compare_options_default(&o);
        got = compare_outputs(sa.data != NULL ? sa.data : "", sa.len, sb.data != NULL ? sb.data : "", sb.len, &o, &approx);
        checks++;
        if (got != want || approx) {
            if (mismatches++ < 5)
                printf("FAIL myers round %d: got %ld want %ld (len %zu/%zu, approx %d)\n", round, got, want, na, nb, approx);
            failures++;
        }
        free(ca);
        free(cb);
        sb_free(&sa);
        sb_free(&sb);
    }

    /* 大一點的 (25M 格)：Myers 與教科書版一致 */
    {
        const size_t n = 5000;
        uint32_t *a = malloc(n * sizeof(uint32_t)), *b = malloc(n * sizeof(uint32_t));
        StrBuf sa = {0}, sb = {0};
        size_t i;
        int approx = 0;
        CompareOptions o;
        compare_options_default(&o);
        for (i = 0; i < n; i++) {
            a[i] = 'a' + rng() % 4;
            b[i] = 'a' + rng() % 4;
        }
        for (i = 0; i < n; i++) {
            char c = (char)a[i];
            sb_append_len(&sa, &c, 1);
            c = (char)b[i];
            sb_append_len(&sb, &c, 1);
        }
        CHECK_LONG(compare_outputs(sa.data, sa.len, sb.data, sb.len, &o, &approx), naive_distance(a, n, b, n));
        CHECK_LONG(approx, 0);
        free(a);
        free(b);
        sb_free(&sa);
        sb_free(&sb);
    }
}

static void test_templates(void)
{
    GradeConfig cfg;
    int i;
    char msg[1024];

    CHECK_LONG(config_template_count() >= 5, 1);
    for (i = 0; i < config_template_count(); i++) {
        config_default(&cfg);
        cfg.full_score = 60;
        cfg.ai_fix = 0;
        config_apply_template(&cfg, i);
        CHECK_DOUBLE(cfg.full_score, 60); /* 模板不改滿分 */
        CHECK_LONG(cfg.ai_fix, 0);        /* 模板也不改 CE 自動修正 */
        CHECK_LONG(score_validate(&cfg.rules, msg, sizeof(msg)), RULES_OK);
    }
    /* 全對才給分：有差異就扣光 */
    config_default(&cfg);
    config_apply_template(&cfg, 5);
    CHECK_DOUBLE(score_apply(20, score_penalty(&cfg.rules, 0)), 20);
    CHECK_DOUBLE(score_apply(20, score_penalty(&cfg.rules, 1)), 0);
}

static void test_roster(void)
{
    char sid[64];
    GradeConfig cfg;

    roster_guess_sid("115403023_peter", sid, sizeof(sid));
    CHECK_STR(sid, "115403023");
    roster_guess_sid("d11230735qian_yi_pei", sid, sizeof(sid));
    CHECK_STR(sid, "d11230735");
    roster_guess_sid("zhang_dun_pin", sid, sizeof(sid));
    CHECK_LONG(sid[0], 0);
    roster_guess_sid("115403025hokaije", sid, sizeof(sid));
    CHECK_STR(sid, "115403025");

    /* 模板判斷：套用後一定認得出來，改一個設定就變「自訂」 */
    config_default(&cfg);
    config_apply_template(&cfg, 2);
    CHECK_LONG(config_match_template(&cfg), 2);
    cfg.compare.ignore_case = 1;
    CHECK_LONG(config_match_template(&cfg), -1);
    config_default(&cfg);
    CHECK_LONG(cfg.timeout_ms, 60000);
}

static void test_testcase_text(void)
{
    char dir[MAX_PATH + 64], err[256];
    const char *text =
        "===== a | 可見 | 一般情況 =====\n"
        "資訊管理學系 一年級\n"
        "\n"
        "===== b | 隱藏 | 英文 =====\n"
        "CS 2\n"
        "----- 標準答案 -----\n"
        "==========您輸入的資料如下==========\n" /* 輸出裡的分隔線不能被當成標題 */
        "系所: CS\n"
        "===== c | hidden | 空輸入 =====\n";
    TestSet set;
    char *exported;
    int n;

    temp_path(dir, sizeof(dir), "grader_tc_test");
    remove_dir_recursive(dir);
    make_dir(dir);

    n = testcase_import_text(dir, text, err, sizeof(err));
    CHECK_LONG(n, 3);
    CHECK_LONG(testcase_scan(dir, &set, err, sizeof(err)), 1);
    CHECK_LONG(set.count, 3);
    CHECK_STR(set.items[0].name, "test01"); /* 重新編號 */
    CHECK_LONG(set.items[0].hidden, 0);
    CHECK_STR(set.items[0].desc, "一般情況");
    CHECK_LONG(set.items[0].has_out, 0);
    CHECK_LONG(set.items[1].hidden, 1);
    CHECK_LONG(set.items[1].has_out, 1);
    CHECK_LONG(set.items[2].hidden, 1);
    {
        size_t len;
        char *in = read_file(set.items[0].in_path, &len);
        CHECK_STR(in, "資訊管理學系 一年級\n"); /* 結尾空白行去掉，保留一個換行 */
        free(in);
        in = read_file(set.items[1].out_path, &len);
        CHECK_STR(in, "==========您輸入的資料如下==========\n系所: CS\n");
        free(in);
        in = read_file(set.items[2].in_path, &len);
        CHECK_LONG((long)len, 0);
        free(in);
    }
    testcase_free(&set);

    /* 匯出再匯入，內容不變 */
    exported = testcase_export_text(dir);
    CHECK_LONG(strstr(exported, "===== test02 | 隱藏 | 英文 =====") != NULL, 1);
    CHECK_LONG(testcase_import_text(dir, exported, err, sizeof(err)), 3);
    {
        char *again = testcase_export_text(dir);
        CHECK_STR(again, exported);
        free(again);
    }
    free(exported);

    /* 沒有標題行 -> 錯誤 */
    CHECK_LONG(testcase_import_text(dir, "abc\n", err, sizeof(err)), -1);
    remove_dir_recursive(dir);
}

static void test_comparator(void)
{
    /* 規格書裡的例子 */
    CHECK_LONG(diff_of("ABCDEFG", "ABCDEFG", COMPARE_STRICT), 0);
    CHECK_LONG(diff_of("ABCDEFG", "ABCXEFG", COMPARE_STRICT), 1);
    CHECK_LONG(diff_of("ABCDEFG", "ABCXYFG", COMPARE_STRICT), 2);
    CHECK_LONG(diff_of("ABCDEFG", "ABC", COMPARE_STRICT), 4);
    CHECK_LONG(diff_of("ABC", "ABCDEFG", COMPARE_STRICT), 4);
    CHECK_LONG(diff_of("", "ABC", COMPARE_STRICT), 3);
    CHECK_LONG(diff_of("ABC", "", COMPARE_STRICT), 3);
    CHECK_LONG(diff_of("Hello World", "Hello  World", COMPARE_STRICT), 1);
    CHECK_LONG(diff_of("10 20 30", "10 25 30", COMPARE_STRICT), 1);
    /* 插入一個字元只算 1，不會讓後面全部錯位 */
    CHECK_LONG(diff_of("ABCDEFG", "ABXCDEFG", COMPARE_STRICT), 1);
    /* 前後相鄰字交換 = 2 (一次刪除 + 一次新增，或兩次替換) */
    CHECK_LONG(diff_of("printf", "pirntf", COMPARE_STRICT), 2);

    /* 中文：1 個中文字 = 1 CHAR */
    CHECK_LONG(diff_of("系所: 資工", "系所: 資管", COMPARE_STRICT), 1);
    CHECK_LONG(diff_of("系所:資工", "系所: 資工", COMPARE_STRICT), 1);
    CHECK_LONG(diff_of("姓名", "", COMPARE_STRICT), 2);

    /* Windows 換行 \r\n 視為 \n；BOM 忽略 */
    CHECK_LONG(diff_of("a\nb\n", "a\r\nb\r\n", COMPARE_STRICT), 0);
    CHECK_LONG(diff_of("abc", "\xEF\xBB\xBF" "abc", COMPARE_STRICT), 0);
    /* Strict 模式下，少最後一個換行算 1 */
    CHECK_LONG(diff_of("abc\n", "abc", COMPARE_STRICT), 1);

    /* Ignore Whitespace */
    CHECK_LONG(diff_of("Hello World", "Hello    World", COMPARE_IGNORE_WHITESPACE), 0);
    CHECK_LONG(diff_of("Hello World\n", "Hello\tWorld", COMPARE_IGNORE_WHITESPACE), 0);
    CHECK_LONG(diff_of("Hello World", "HelloWorld", COMPARE_IGNORE_WHITESPACE), 0);
    CHECK_LONG(diff_of("1 2 3", "1 2 4", COMPARE_IGNORE_WHITESPACE), 1);
    CHECK_LONG(diff_of("系所: 資工", "系所:資工", COMPARE_IGNORE_WHITESPACE), 0);

    /* Token */
    CHECK_LONG(diff_of("10 20 30", "10    20    30", COMPARE_TOKEN), 0);
    CHECK_LONG(diff_of("10 20 30\n", "  10\n20\t30  ", COMPARE_TOKEN), 0);
    CHECK_LONG(diff_of("Hello World", "HelloWorld", COMPARE_TOKEN), 1); /* token 數不同就算差異 */
    CHECK_LONG(diff_of("10 20 30", "10 25 30", COMPARE_TOKEN), 1);

    /* 第一個不同處 */
    {
        long line, col;
        CHECK_LONG(compare_first_difference("ab\ncd", 5, "ab\ncx", 5, &line, &col), 1);
        CHECK_LONG(line, 2);
        CHECK_LONG(col, 2);
        CHECK_LONG(compare_first_difference("ab", 2, "ab", 2, &line, &col), 0);
    }
}

static void test_per_char(void)
{
    ScoreRules r;
    memset(&r, 0, sizeof(r));
    r.mode = SCORE_PER_CHAR;

    r.per_char_penalty = 1;
    CHECK_DOUBLE(score_penalty(&r, 0), 0);
    CHECK_DOUBLE(score_penalty(&r, 1), 1);
    CHECK_DOUBLE(score_penalty(&r, 2), 2);
    CHECK_DOUBLE(score_penalty(&r, 10), 10);
    CHECK_DOUBLE(score_penalty(&r, 100), 100);

    r.per_char_penalty = 0.5;
    CHECK_DOUBLE(score_penalty(&r, 10), 5);
    CHECK_DOUBLE(score_penalty(&r, 3), 1.5);
}

static void test_per_n_char(void)
{
    ScoreRules r;
    const long diffs[] = {0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 20, 21, 100};
    const double want[] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 3, 6, 7, 33};
    int i;

    memset(&r, 0, sizeof(r));
    r.mode = SCORE_PER_N_CHAR;
    r.n_chars = 3;
    r.per_n_penalty = 1;
    for (i = 0; i < (int)(sizeof(diffs) / sizeof(diffs[0])); i++)
        CHECK_DOUBLE(score_penalty(&r, diffs[i]), want[i]);

    r.per_n_penalty = 2.5;
    CHECK_DOUBLE(score_penalty(&r, 7), 5);
}

static void test_range(void)
{
    GradeConfig cfg;
    ScoreRules r;
    const long diffs[] = {0, 1, 2, 3, 5, 6, 10, 11, 20, 21, 100};
    const double want[] = {0, 1, 1, 2, 2, 5, 5, 10, 10, 20, 20};
    char msg[1024];
    int i;

    config_default(&cfg); /* 預設就是規格書的區間表 */
    CHECK_LONG(score_validate(&cfg.rules, NULL, 0), RULES_OK);
    for (i = 0; i < (int)(sizeof(diffs) / sizeof(diffs[0])); i++)
        CHECK_DOUBLE(score_penalty(&cfg.rules, diffs[i]), want[i]);

    /* 最低 0 分 */
    CHECK_DOUBLE(score_apply(100, 150), 0);
    CHECK_DOUBLE(score_apply(20, 2), 18);

    /* 區間表忘了寫 0～0：差異 0 (全對) 仍然不扣分，而且不算空缺 */
    memset(&r, 0, sizeof(r));
    r.mode = SCORE_RANGE;
    r.range_count = 1;
    r.ranges[0] = (RangeRule){1, RANGE_MAX, 5};
    CHECK_DOUBLE(score_penalty(&r, 0), 0);
    CHECK_DOUBLE(score_penalty(&r, 1), 5);
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_OK);
    r.ranges[0].min = 3; /* 1～2 沒有規則才是空缺 */
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_WARNING);
    CHECK_LONG(strstr(msg, "1～2") != NULL, 1);
}

static void test_validate(void)
{
    ScoreRules r;
    char msg[1024];

    memset(&r, 0, sizeof(r));
    r.mode = SCORE_RANGE;

    /* 重疊：1～5 與 4～10 */
    r.range_count = 2;
    r.ranges[0] = (RangeRule){1, 5, 1};
    r.ranges[1] = (RangeRule){4, 10, 2};
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_ERROR);
    CHECK_LONG(strstr(msg, "重疊") != NULL, 1);

    /* 最小值大於最大值：10～5 */
    r.range_count = 1;
    r.ranges[0] = (RangeRule){10, 5, 1};
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_ERROR);
    CHECK_LONG(strstr(msg, "最小值不能大於最大值") != NULL, 1);

    /* MAX 與後面的區間重疊 */
    r.range_count = 2;
    r.ranges[0] = (RangeRule){0, RANGE_MAX, 1};
    r.ranges[1] = (RangeRule){5, 10, 2};
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_ERROR);

    /* 空缺：0～5、7～MAX -> 預設只是提醒 */
    r.range_count = 2;
    r.ranges[0] = (RangeRule){0, 5, 1};
    r.ranges[1] = (RangeRule){7, RANGE_MAX, 2};
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_WARNING);
    CHECK_LONG(strstr(msg, "6～6") != NULL, 1);
    /* 空缺中的差異數：扣光 */
    CHECK_DOUBLE(score_apply(20, score_penalty(&r, 6)), 0);

    /* 要求連續 -> 空缺變成錯誤 */
    r.require_contiguous = 1;
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_ERROR);

    /* 沒有規則 */
    r.range_count = 0;
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_ERROR);

    /* 每 N CHAR：N 必須 > 0 */
    r.mode = SCORE_PER_N_CHAR;
    r.n_chars = 0;
    CHECK_LONG(score_validate(&r, msg, sizeof(msg)), RULES_ERROR);
}

static void test_config_roundtrip(void)
{
    GradeConfig a, b;
    char path[MAX_PATH + 32], err[256];

    temp_path(path, sizeof(path), "grader_test.cfg");

    config_default(&a);
    a.full_score = 60;
    a.compare.mode = COMPARE_TOKEN;
    a.compare.fullwidth_as_halfwidth = 1;
    snprintf(a.compare.ignore_lines_with, sizeof(a.compare.ignore_lines_with), "請輸入|請依序");
    a.rules.mode = SCORE_PER_N_CHAR;
    a.rules.n_chars = 4;
    a.rules.per_n_penalty = 0.5;
    a.rules.range_count = 2;
    a.rules.ranges[0] = (RangeRule){0, 3, 0};
    a.rules.ranges[1] = (RangeRule){4, RANGE_MAX, 7.5};
    a.ai_fix = 0;
    snprintf(a.ai_fix_tool, sizeof(a.ai_fix_tool), "codex");
    a.ai_fix_penalty_per_char = 0.5;
    a.ai_fix_penalty_max = 20;
    a.ai_fix_attempts = 3;
    a.ai_fix_timeout_ms = 90000;
    CHECK_LONG(config_save(path, &a), 1);

    config_default(&b);
    CHECK_LONG(config_load(path, &b, err, sizeof(err)), 1);
    CHECK_DOUBLE(b.full_score, 60);
    CHECK_LONG(b.compare.mode, COMPARE_TOKEN);
    CHECK_LONG(b.compare.fullwidth_as_halfwidth, 1);
    CHECK_STR(b.compare.ignore_lines_with, "請輸入|請依序");
    CHECK_LONG(b.rules.mode, SCORE_PER_N_CHAR);
    CHECK_LONG(b.rules.n_chars, 4);
    CHECK_DOUBLE(b.rules.per_n_penalty, 0.5);
    CHECK_LONG(b.rules.range_count, 2);
    CHECK_LONG(b.rules.ranges[1].max, RANGE_MAX);
    CHECK_DOUBLE(b.rules.ranges[1].penalty, 7.5);
    CHECK_LONG(b.ai_fix, 0);
    CHECK_STR(b.ai_fix_tool, "codex");
    CHECK_DOUBLE(b.ai_fix_penalty_per_char, 0.5);
    CHECK_DOUBLE(b.ai_fix_penalty_max, 20);
    CHECK_LONG(b.ai_fix_attempts, 3);
    CHECK_LONG(b.ai_fix_timeout_ms, 90000);
    DeleteFileA(path);

    /* 值裡的 # 不是註解；行尾 (前面有空白) 的 # 才是 */
    {
        const char *text =
            "\xEF\xBB\xBF# 註解\n"
            "ignore_lines_with = C#|請輸入   # 這才是註解\n"
            "compile_flags = -lm -DVER=1#2\n"
            "ai_fix = yes\n"
            "ai_fix_tool =\n";
        write_file(path, text, strlen(text));
        config_default(&b);
        CHECK_LONG(config_load(path, &b, err, sizeof(err)), 1);
        CHECK_STR(b.compare.ignore_lines_with, "C#|請輸入");
        CHECK_STR(b.compile_flags, "-lm -DVER=1#2");
        CHECK_LONG(b.ai_fix, 1);
        CHECK_STR(b.ai_fix_tool, "auto"); /* 空的就回到 auto */
        DeleteFileA(path);
    }

    /* 修正扣分：每 CHAR x 上限 */
    config_default(&a);
    a.ai_fix_penalty_per_char = 2;
    a.ai_fix_penalty_max = 0;
    CHECK_DOUBLE(config_fix_penalty(&a, 0), 0);
    CHECK_DOUBLE(config_fix_penalty(&a, -1), 0);
    CHECK_DOUBLE(config_fix_penalty(&a, 7), 14);
    a.ai_fix_penalty_max = 10;
    CHECK_DOUBLE(config_fix_penalty(&a, 7), 10);
    CHECK_DOUBLE(config_fix_penalty(&a, 3), 6);
}

static void test_ai_fix_helpers(void)
{
    char *code, *prompt, name[AI_TOOL_NAME_MAX], command[AI_COMMAND_MAX], dir[MAX_PATH + 64], err[512];

    /* 有圍欄：只取第一個圍欄裡的內容 */
    code = ai_extract_code("這是修正後的程式：\n```c\n#include <stdio.h>\nint main(void) { return 0; }\n```\n說明文字\n");
    CHECK_STR(code, "#include <stdio.h>\nint main(void) { return 0; }\n");
    free(code);
    /* 沒有圍欄：整段，\r\n 換成 \n，結尾一個換行 */
    code = ai_extract_code("#include <stdio.h>\r\nint main(void){return 0;}\r\n\r\n");
    CHECK_STR(code, "#include <stdio.h>\nint main(void){return 0;}\n");
    free(code);
    /* BOM、只有開頭圍欄 */
    code = ai_extract_code("\xEF\xBB\xBF```\nint x;");
    CHECK_STR(code, "int x;\n");
    free(code);
    /* 空回答 */
    code = ai_extract_code("  \n\n");
    CHECK_LONG(code == NULL, 1);

    /* 提示詞 */
    prompt = ai_fix_prompt("main.c", "int main(){}", "main.c:1: error: x", 2);
    CHECK_LONG(strstr(prompt, "第 2 次") != NULL, 1);
    CHECK_LONG(strstr(prompt, "main.c:1: error: x") != NULL, 1);
    CHECK_LONG(strstr(prompt, "int main(){}\n") != NULL, 1);
    CHECK_LONG(strstr(prompt, "不要 Markdown") != NULL, 1);
    free(prompt);

    /* 工具：自訂命令列 */
    CHECK_LONG(ai_tool_find("cmd /c echo hi", name, sizeof(name), command, sizeof(command)), 1);
    CHECK_STR(name, "cmd");
    CHECK_STR(command, "cmd /c echo hi");

    /* 實際透過 cmd 走一趟 stdin -> stdout (sort 會原樣輸出單行) */
    temp_path(dir, sizeof(dir), "grader_ai_test");
    make_dir(dir);
    code = ai_ask("sort", "hello grader\n", dir, 20000, err, sizeof(err));
    CHECK_LONG(code != NULL, 1);
    if (code != NULL) {
        char *extracted = ai_extract_code(code);
        CHECK_STR(extracted, "hello grader\n");
        free(extracted);
        free(code);
    } else {
        printf("      ai_ask err: %s\n", err);
    }
    /* 失敗的命令：回傳 NULL 並說明 */
    code = ai_ask("cmd /c exit 3", "x", dir, 20000, err, sizeof(err));
    CHECK_LONG(code == NULL, 1);
    CHECK_LONG(strstr(err, "3") != NULL, 1);
    remove_dir_recursive(dir);
}

static void test_status_and_csv(void)
{
    Grader g;
    StudentResult r[2];
    char path[MAX_PATH + 32], err[256], *text;

    CHECK_LONG(student_ran_tests(STUDENT_OK), 1);
    CHECK_LONG(student_ran_tests(STUDENT_CE_FIXED), 1);
    CHECK_LONG(student_ran_tests(STUDENT_COMPILE_ERROR), 0);
    CHECK_LONG(student_ran_tests(STUDENT_NO_SOURCE), 0);
    CHECK_STR(student_status_name(STUDENT_CE_FIXED), "CE Fixed");

    memset(&g, 0, sizeof(g));
    memset(r, 0, sizeof(r));
    snprintf(r[0].id, sizeof(r[0].id), "=cmd|' /C calc'!A0"); /* Excel 公式注入 + 含逗號的姓名 */
    snprintf(r[0].name, sizeof(r[0].name), "王,小明");
    r[0].status = STUDENT_OK;
    r[0].score = 100;
    snprintf(r[1].id, sizeof(r[1].id), "s2");
    r[1].status = STUDENT_CE_FIXED;
    r[1].fix_chars = 3;
    r[1].fix_penalty = 3;
    r[1].score = 50;
    r[1].total_deduction = 50;
    r[1].total_diff = 7;

    temp_path(path, sizeof(path), "grader_test.csv");
    CHECK_LONG(export_csv(path, &g, r, 2, err, sizeof(err)), 1);
    text = read_file(path, NULL);
    CHECK_LONG(text != NULL, 1);
    if (text != NULL) {
        CHECK_LONG(strstr(text, "FixChars,FixPenalty,Score") != NULL, 1);
        CHECK_LONG(strstr(text, "\"'=cmd|' /C calc'!A0\",\"王,小明\",") != NULL, 1); /* ' 在引號裡面 */
        CHECK_LONG(strstr(text, "s2,,s2,CE Fixed,0,0,7,50,3,3,50,") != NULL, 1);
        free(text);
    }
    DeleteFileA(path);
}

static void test_util(void)
{
    char path[MAX_PATH + 32], *buf, big[4096];
    size_t len = 0;
    int truncated = 0, i;

    temp_path(path, sizeof(path), "grader_util_test.bin");
    for (i = 0; i < (int)sizeof(big); i++)
        big[i] = (char)('A' + i % 26);
    CHECK_LONG(write_file(path, big, sizeof(big)), 1);
    buf = read_file_limited(path, 1000, &len, &truncated);
    CHECK_LONG((long)len, 1000);
    CHECK_LONG(truncated, 1);
    free(buf);
    buf = read_file_limited(path, 0, &len, &truncated);
    CHECK_LONG((long)len, (long)sizeof(big));
    CHECK_LONG(truncated, 0);
    free(buf);
    CHECK_LONG(delete_file(path), 1);
    CHECK_LONG(delete_file(path), 1); /* 不存在也算成功 */
    CHECK_LONG(file_exists(path), 0);
}

int main(void)
{
    SetConsoleOutputCP(CP_UTF8);
    test_comparator();
    test_long_outputs();
    test_myers_random();
    test_per_char();
    test_per_n_char();
    test_range();
    test_validate();
    test_config_roundtrip();
    test_compare_options();
    test_templates();
    test_roster();
    test_testcase_text();
    test_ai_fix_helpers();
    test_status_and_csv();
    test_util();
    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
