/*
 * main.c - GUI 版入口。
 * 命令列版入口在 cli_main.c，兩者共用同一組核心模組。
 */
#include <windows.h>

#include "gui.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmd_line, int show)
{
    (void)prev;
    (void)cmd_line;
    return gui_run(instance, show);
}
