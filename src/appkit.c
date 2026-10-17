#include <string.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <ToolUtils.h>

#include "appkit.h"
#include "ddp.h"
#include "macip.h"
#include "tcp.h"
#include "fmt.h"

#define AK_LOG_LINES 16
#define AK_LOG_WIDTH 80

static MenuHandle apple_menu, file_menu, edit_menu;
static short quit_item;
static char log_text[AK_LOG_LINES][AK_LOG_WIDTH];
static Boolean log_changed;
static MacIPState last_state = MACIP_DOWN;

static void resume_proc(void)
{
    ExitToShell();      /* ddp.c patches it to close socket 72 first */
}

void ak_init(void)
{
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs((void *)resume_proc);
    InitCursor();
}

void ak_menus(const char *about, const char *file_items)
{
    Str255 p;

    apple_menu = NewMenu(AK_APPLE_MENU, "\p\024");
    AppendMenu(apple_menu, fmt_pstr(p, about));
    AppendMenu(apple_menu, "\p(-");
    AppendResMenu(apple_menu, 'DRVR');
    InsertMenu(apple_menu, 0);

    file_menu = NewMenu(AK_FILE_MENU, "\pFile");
    if (file_items && *file_items) {
        AppendMenu(file_menu, fmt_pstr(p, file_items));
        AppendMenu(file_menu, "\p(-");
    }
    AppendMenu(file_menu, "\pQuit/Q");
    quit_item = CountMItems(file_menu);
    InsertMenu(file_menu, 0);

    edit_menu = NewMenu(AK_EDIT_MENU, "\pEdit");
    AppendMenu(edit_menu, "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear");
    InsertMenu(edit_menu, 0);
    DrawMenuBar();
}

short ak_file_quit_item(void)
{
    return quit_item;
}

Boolean ak_menu_common(long choice, short *file_item, Boolean *about, Boolean *quit)
{
    short menu = HiWord(choice), item = LoWord(choice);

    *file_item = 0;
    *about = false;
    *quit = false;
    if (menu == AK_APPLE_MENU) {
        if (item == 1) {
            *about = true;
        } else {
            Str255 name;
            GetMenuItemText(apple_menu, item, name);
            OpenDeskAcc(name);
        }
        return true;
    }
    if (menu == AK_FILE_MENU) {
        if (item == quit_item)
            *quit = true;
        else
            *file_item = item;
        return true;
    }
    if (menu == AK_EDIT_MENU) {
        /* a desk accessory in front takes it; otherwise the app does Cut/Copy/Paste in its own fields */
        return SystemEdit(item - 1);
    }
    return false;
}

void ak_log(const char *a, const char *b)
{
    int i;
    char *line;

    for (i = 0; i < AK_LOG_LINES - 1; i++)
        strcpy(log_text[i], log_text[i + 1]);
    line = log_text[AK_LOG_LINES - 1];
    line[0] = '\0';
    strncat(line, a, AK_LOG_WIDTH - 1);
    if (b)
        strncat(line, b, AK_LOG_WIDTH - 1 - strlen(line));
    log_changed = true;
}

Boolean ak_log_changed(void)
{
    Boolean c = log_changed;

    log_changed = false;
    return c;
}

void ak_draw_text(short h, short v, const char *s)
{
    Str255 p;

    MoveTo(h, v);
    DrawString(fmt_pstr(p, s));
}

void ak_draw_log(short left, short top, short lines)
{
    int i, first = AK_LOG_LINES - lines;

    if (first < 0)
        first = 0;
    TextFont(4);        /* Monaco */
    TextSize(9);
    for (i = first; i < AK_LOG_LINES; i++)
        ak_draw_text(left, top + 10 + (i - first) * 11, log_text[i]);
}

Boolean ak_net_start(void)
{
    OSErr err;
    char b[24] = "";

    err = ddp_open();
    if (err != noErr) {
        fmt_err(b, err);
        ak_log("Can't open AppleTalk socket 72: error ", b);
        return false;
    }
    ak_log("Looking for a MacIP gateway", NULL);
    if (macip_find_gateway() != noErr || macip_assign() != noErr) {
        ak_log(macip_error, ", will retry");
        macip_retry_later();
        last_state = macip_state;
        return false;
    }
    ak_log("Leased ", fmt_ip(b, macip_cfg.ip));
    last_state = macip_state;
    return true;
}

void ak_net_poll(void)
{
    macip_poll();
    tcp_poll();
    if (macip_state != last_state) {
        if (macip_state == MACIP_UP) {
            char b[20] = "";
            ak_log("Leased ", fmt_ip(b, macip_cfg.ip));
        } else if (macip_error[0]) {
            ak_log(macip_error, NULL);
        }
        last_state = macip_state;
    }
}

void ak_net_stop(void)
{
    ddp_close();
}

void ak_net_status(char *b)
{
    b[0] = '\0';
    if (macip_state == MACIP_UP) {
        fmt_str(b, "MacIP up: ");
        fmt_ip(b, macip_cfg.ip);
        fmt_str(b, " via ");
        fmt_str(b, macip_cfg.gateway_name);
    } else if (macip_recovering()) {
        fmt_str(b, "MacIP: looking for the gateway...");
    } else {
        fmt_str(b, "MacIP: down");
    }
}
