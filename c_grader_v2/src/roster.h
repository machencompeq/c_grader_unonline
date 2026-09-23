/*
 * roster.h - 學生名單：把下載後的資料夾名稱 (英文拼音，例如 zhang_dun_pin) 對應回中文姓名與學號。
 *
 * 名單檔是 CSV (可用 Excel 或記事本編輯)，放在批改工具資料夾 (Grader.exe 旁邊) 的 名單.csv：
 *
 *   資料夾名稱,姓名,學號
 *   zhang_dun_pin,張敦品,
 *   115403023_peter,,115403023
 *
 * 教學平台下載時會把中文姓名轉成拼音，無法自動反推 (同音字太多)，
 * 所以由老師填一次姓名，之後每一題都會用同一份名單。
 * Excel 另存的 CSV 若是 Big5 (ANSI) 也能讀。
 */
#ifndef ROSTER_H
#define ROSTER_H

#include <stddef.h>

#include "util.h"

#define ROSTER_FILE_NAME "名單.csv"

/* 名單放在批改工具資料夾 (Grader.exe 旁邊)，全班共用、每一題都能用 */
void roster_default_path(char *out, size_t size);

typedef struct {
    char folder[256];  /* 資料夾名稱 */
    char name[128];    /* 中文姓名 (可空白) */
    char sid[64];      /* 學號 (可空白) */
} RosterEntry;

typedef struct {
    RosterEntry *items;
    int count;
} Roster;

/* 讀取名單；檔案不存在回傳 0 (roster 為空) */
int roster_load(const char *path, Roster *roster);
void roster_free(Roster *roster);

/* 找某個資料夾的資料 (不分大小寫)，找不到回傳 NULL */
const RosterEntry *roster_find(const Roster *roster, const char *folder);

/*
 * 產生 / 更新名單檔：列出 folders 裡的每個資料夾，
 * 已經填過的姓名學號保留；新的資料夾自動推測學號 (資料夾名稱開頭的學號)。
 * 回傳新增的資料夾數量，失敗回傳 -1。
 */
int roster_sync(const char *path, const StringList *folders);

/* 從資料夾名稱推測學號：115403023_peter -> 115403023、d11230735qian_yi_pei -> d11230735 */
void roster_guess_sid(const char *folder, char *out, size_t size);

/* 顯示用名稱：「姓名 (學號)」或資料夾名稱 */
void roster_display_name(const Roster *roster, const char *folder, char *out, size_t size);

#endif
