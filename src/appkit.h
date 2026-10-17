/*
 * appkit.h - what every talkip tool shares: Toolbox start-up (with a resume procedure that exits through
 * ExitToShell, so a crash still closes socket 72), Apple/File/Edit menus, a log pane, and bringing the
 * network up (socket 72, gateway lookup, lease) with the stack's retry loop behind it.
 */
#ifndef TALKIP_APPKIT_H
#define TALKIP_APPKIT_H

#include <MacTypes.h>
#include <Events.h>
#include <Windows.h>

#define AK_APPLE_MENU   128
#define AK_FILE_MENU    129
#define AK_EDIT_MENU    130

void ak_init(void);                                 /* MaxApplZone + the Init* calls */
/* about: the Apple menu's first item text; file_items: AppendMenu text for File (Quit is added last). */
void ak_menus(const char *about, const char *file_items);
/* Apple menu and Edit menu (via SystemEdit for desk accessories). Returns true if it handled the choice;
 * *about is set when "About" was chosen, *quit when File's last item (Quit) was. */
Boolean ak_menu_common(long choice, short *file_item, Boolean *about, Boolean *quit);
short ak_file_quit_item(void);

/* Log pane: the last lines, drawn in Monaco 9 from (left, top) downward. */
void ak_log(const char *a, const char *b);
void ak_draw_log(short left, short top, short lines);
Boolean ak_log_changed(void);                       /* true once after each ak_log */

/* Network: open socket 72, find the gateway, lease. Logs each step. Returns true if up now; if not, the
 * stack keeps retrying from ak_net_poll. */
Boolean ak_net_start(void);
void ak_net_poll(void);                             /* macip_poll + logs state changes */
void ak_net_stop(void);                             /* closes socket 72 */
void ak_net_status(char *buf);                      /* "up 192.168.72.3 via 192.168.72.1" etc. */

void ak_draw_text(short h, short v, const char *s);

#endif
