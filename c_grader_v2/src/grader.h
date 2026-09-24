/*
 * grader.h - 批改流程：參考程式 -> 每位學生 (編譯 -> 執行 testcase -> 比對 -> 評分)。
 *
 * 兩個資料夾：
 *   題目資料夾   reference 資料夾 (main.c) + testcase 資料夾 (.in .out) (+ grader.cfg、reports/)
 *   學生資料夾   <學號> 裡的 .c 檔 (就是從教學平台下載、解壓縮後的資料夾)
 * 舊格式 Homework/students/<學號> 也支援：學生資料夾給 Homework 即可。
 *
 * 使用方式 (CLI 與 GUI 都一樣)：
 *
 *   Grader g;
 *   grader_open(&g, problem_dir, students_dir, &cfg, err, sizeof err);
 *   grader_prepare_reference(&g, &report, err, sizeof err);   // 編譯並驗證參考程式
 *   for (i = 0; i < g.students.count; i++)
 *       grader_grade_student(&g, i, &results[i]);
 *   grader_close(&g);
 *
 * 編譯失敗 (CE) 的學生：設定 ai_fix 開啟時，把原始碼 + gcc 錯誤訊息交給本機 AI 做最小修正 (ai_fix.c)，
 * 修正後照常執行測資；成績 = max(各題得分總和 − 修正扣分, min(各題得分總和, 保底分))，
 * 修正扣分 = max(修正字元數 × 每 CHAR 扣分, 下限)。修不好 / 改太多 / 沒有 AI 工具 -> 保底分 (不會是 0 分)。
 */
#ifndef GRADER_H
#define GRADER_H

#include "ai_fix.h"
#include "config.h"
#include "executor.h"
#include "roster.h"
#include "testcase.h"
#include "util.h"

#define OUTPUT_PREVIEW_LIMIT (64 * 1024) /* 保留多少學生輸出給畫面/報告顯示 */

typedef enum {
    TEST_PASS,          /* 差異數 0 */
    TEST_WRONG,         /* 有差異 */
    TEST_RUNTIME_ERROR,
    TEST_TIMEOUT,
    TEST_OUTPUT_LIMIT,
    TEST_NOT_RUN        /* 編譯失敗等原因沒有執行 */
} TestStatus;

typedef struct {
    TestStatus status;
    long diff;          /* 差異數，-1 = 沒有比對 */
    int approximate;    /* 1 = 差異數為近似值 (輸出過長) */
    double full;        /* 這題滿分 */
    double deduction;   /* 實際扣掉的分數 (最多扣到這題滿分) */
    double score;       /* 這題得分 */
    int elapsed_ms;
    unsigned long exit_code;
    char *actual;       /* 學生輸出 (最多 OUTPUT_PREVIEW_LIMIT bytes) */
    size_t actual_len;
    int actual_truncated;
} TestResult;

typedef enum {
    STUDENT_OK,
    STUDENT_CE_FIXED,      /* 編譯失敗，但 AI 最小修正後可以編譯，已照常執行測資 */
    STUDENT_COMPILE_ERROR,
    STUDENT_NO_SOURCE
} StudentStatus;

/* 有沒有執行測資 (OK 或 CE_FIXED)：報告、CSV、畫面判斷「要不要顯示各題結果」都用這個 */
int student_ran_tests(StudentStatus status);

typedef struct {
    char id[256];        /* 資料夾名稱 */
    char name[128];      /* 中文姓名 (名單.csv；沒填就空白) */
    char sid[64];        /* 學號 (名單.csv，或從資料夾名稱推測) */
    StudentStatus status;
    StringList sources;  /* 實際編譯的 .c 檔 (完整路徑) */
    char note[512];      /* 批改工具自動處理的說明，例如「原始碼為 Big5，已自動轉換」 */
    char *compile_log;   /* 學生原始碼的 gcc 訊息 (CE 時是錯誤訊息) */
    TestResult *tests;   /* 長度 = testcase 數量 */
    int test_count;
    int passed;
    int failed;
    long total_diff;     /* -1 = 無法計算 */
    double total_deduction; /* 輸出扣分 + 修正扣分 (封頂於滿分) */
    double score;

    /* 編譯失敗的 AI 最小修正 (status == STUDENT_CE_FIXED 才會計分；CE 時若有嘗試也會留下最後一次的結果) */
    char *fixed_source;  /* 修正後原始碼 (UTF-8)，NULL = 沒有嘗試 */
    char *fix_log;       /* 修正後程式的 gcc 訊息 */
    long fix_chars;      /* 修正字元數 (原始碼 -> 修正後 的編輯距離)，-1 = 沒有修正 */
    int fix_approximate;
    double fix_penalty;  /* 修正扣分 */
    int fix_attempts;    /* AI 試了幾次 */
    char fix_tool[AI_TOOL_NAME_MAX]; /* 實際使用的工具 (claude / codex / gemini / 自訂) */
    int fix_cached;      /* 1 = 沿用上次的修正 (快取) */
    int fix_rejected;    /* 1 = AI 修好了但改太多字，不採用 (給保底分，請老師確認) */
    int ce_floor_applied; /* 1 = 成績是編譯失敗保底分 (或被保底分擋住沒有再往下扣) */
} StudentResult;

typedef struct {
    char name[256];
    int has_out;
    RunStatus run_status;
    long diff;           /* 參考程式輸出 vs .out，-1 = 沒有 .out 或沒有執行 */
} ReferenceCheck;

typedef struct {
    int compiled;
    char *compile_log;
    ReferenceCheck *checks;
    int count;
    int mismatches;      /* 與 .out 不一致的 testcase 數 */
    int failures;        /* 參考程式執行失敗 (RE/TLE...) 的 testcase 數 */
} ReferenceReport;

typedef struct {
    char reference[GRADER_PATH_MAX];     /* 參考答案：.c 檔 (或舊格式的 reference 資料夾) */
    char problem_dir[GRADER_PATH_MAX];   /* 測資資料夾：testcase、grader.cfg、reports */
    char students_dir[GRADER_PATH_MAX];  /* 每個子資料夾 = 一位學生 */
    char temp_dir[GRADER_PATH_MAX];      /* %TEMP%\c-grader\run-xxxx */
    char stdout_helper[GRADER_PATH_MAX]; /* 一起編譯的小檔案：讓 stdout 不暫存 (見 grader.c) */
    GradeConfig cfg;
    TestSet tests;
    StringList students;   /* 學生資料夾名稱 */
    Roster roster;         /* 名單.csv：資料夾名稱 -> 姓名、學號 */
    char **expected;       /* 每個 testcase 的標準答案 */
    size_t *expected_len;
    int reference_ready;

    /* AI 工具只找一次 (第一位 CE 學生時) */
    int ai_checked;
    int ai_available;
    char ai_tool[AI_TOOL_NAME_MAX];
    char ai_command[AI_COMMAND_MAX];
} Grader;

/* 學生資料夾：若 folder 底下有 students/ 就用它，否則 folder 本身就是學生資料夾 */
void grader_resolve_students_dir(const char *folder, char *out, size_t size);

/*
 * 題目 -> 參考答案與測資資料夾：
 *   D:\hw\hw1.c      -> reference = D:\hw\hw1.c、data_dir = D:\hw\hw1_測資
 *   D:\hw\題目資料夾 -> reference = 題目資料夾\reference、data_dir = 題目資料夾 (舊格式)
 */
void grader_problem_paths(const char *problem, char *reference, char *data_dir, size_t size);

/* problem = 參考答案 .c 檔 (或舊格式題目資料夾)；students_dir 可以是 NULL (= 舊格式的 題目資料夾\students) */
int grader_open(Grader *g, const char *problem, const char *students_dir, const GradeConfig *cfg,
                char *err, size_t err_size);
/* 同上，但測資從 data_dir 讀 (NULL = 預設位置)；「編輯測資」預覽輸出時用暫存資料夾 */
int grader_open_ex(Grader *g, const char *problem, const char *data_dir, const char *students_dir,
                   const GradeConfig *cfg, char *err, size_t err_size);

/*
 * 編譯 reference 資料夾裡的 .c，並用每個 testcase 執行一次：
 *   - 有 .out  -> 以 .out 為標準答案，同時檢查參考程式輸出是否一致 (記錄在 report)
 *   - 沒有 .out -> 以參考程式輸出為標準答案
 * 回傳 0 表示無法繼續批改 (參考程式編譯失敗等)。
 */
int grader_prepare_reference(Grader *g, ReferenceReport *report, char *err, size_t err_size);

void grader_grade_student(Grader *g, int index, StudentResult *result);

/* 列出學生資料夾裡的學生 (排除 reference、testcase、reports 等) */
int grader_list_students(const char *students_dir, StringList *out);
/* 找某位學生的 .c 檔 (第一層沒有就往子資料夾找)；relative = 子資料夾相對路徑 */
void grader_find_student_sources(const char *student_dir, StringList *sources, char *relative, size_t size);

/* 刪掉暫存資料夾 (批改完就可以呼叫；標準答案仍留在記憶體中給畫面顯示) */
void grader_remove_temp(Grader *g);
void grader_close(Grader *g);
void reference_report_free(ReferenceReport *report);
void student_result_free(StudentResult *result);

const char *test_status_name(TestStatus status);  /* "AC" "WA" "RE" "TLE" "OLE" "-" */
const char *test_status_text(TestStatus status);  /* 中文說明 */
const char *student_status_name(StudentStatus status); /* "OK" "CE Fixed" "Compile Error" "No Source" */
const char *student_status_text(StudentStatus status); /* 中文：「OK」「CE→AI 修正」「編譯失敗」「沒有 .c 檔」 */

#endif
