/*
 * testcase.h - 測資：<資料夾>\testcase\ 底下的 test01.in (與可省略的 test01.out)。
 *
 * 每組測資另外有「可見 / 隱藏」與「說明」，存在 testcase\meta.txt：
 *   test01|visible|一般情況
 *   test02|hidden|最長的系所名稱
 * 隱藏測資照常計分，但 HTML 報告 (可能發給學生) 不會顯示它的輸入與答案。
 *
 * 老師編輯測資用「總表」文字 (所有測資放在同一段文字，一眼看完、可整批修改)：
 *
 *   ===== test01 | 可見 | 一般情況 =====
 *   資訊管理學系 一年級 甲班 115403001 王小明
 *   ===== test02 | 隱藏 | 英文輸入 =====
 *   CS 2 B 12345 Amy
 *   ----- 標準答案 -----            (可省略；省略時用參考答案的輸出)
 *   ...
 *
 * AI 產生測資的腳本 (tools\ai_testcases.ps1) 也輸出同樣的格式。
 */
#ifndef TESTCASE_H
#define TESTCASE_H

#include <stddef.h>

#include "util.h"

typedef struct {
    char name[256];                 /* "test01" */
    char in_path[GRADER_PATH_MAX];
    char out_path[GRADER_PATH_MAX];
    int has_out;                    /* 1 = 有 test01.out */
    int hidden;                     /* 1 = 隱藏測資 */
    char desc[256];                 /* 說明 */
} TestCase;

typedef struct {
    TestCase *items;
    int count;
} TestSet;

/* data_dir = 測資資料夾的上一層 (裡面有 testcase\) */
int testcase_scan(const char *data_dir, TestSet *set, char *err, size_t err_size);
void testcase_free(TestSet *set);

/* 目前所有測資 -> 總表文字 (malloc)；沒有測資時回傳一組空白範例 */
char *testcase_export_text(const char *data_dir);

/*
 * 總表文字 -> 重新寫入 testcase\ (依出現順序重新編號 test01、test02…)。
 * 原本的 test*.in / test*.out / meta.txt 會被取代。成功回傳測資數量，格式錯誤回傳 -1 並寫入 err。
 */
int testcase_import_text(const char *data_dir, const char *text, char *err, size_t err_size);

#endif
