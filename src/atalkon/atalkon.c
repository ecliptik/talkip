/*
 * atalkon.c - "AppleTalk On": an INIT that turns AppleTalk on at every startup.
 *
 * Whether AppleTalk is active is kept in parameter RAM, in SPConfig: the low nibble is the printer port's use,
 * useATalk (1) when the Chooser says "Active". A Mac without a PRAM battery (a bare 512Ke board) loses it at every
 * power-off and comes up Inactive, so AppleShare, the Chooser and talkip find no AppleTalk until it is turned on
 * again by hand.
 *
 * At startup this sets the printer port's use to AppleTalk in SPConfig (low memory $1FB) and writes it back to
 * PRAM. INITs run before any application opens .MPP, so the Chooser and the first MPPOpen see AppleTalk active.
 * The modem port's nibble is left alone: the Chooser writes $22 for Inactive and $21 for Active, and this makes
 * the same change. Nothing stays resident: the INIT is released after it runs.
 *
 * Tested in Snow (Early512Ke, System 6.0.8, 2026-10-19): booting from Inactive PRAM, the Chooser shows Active and
 * AppleShare lists the server; of the 256 PRAM bytes only SPConfig changes ($22 -> $21).
 */
#include <OSUtils.h>

enum { kUseATalk = 1 };                 /* SPConfig port use: useFree 0, useATalk 1, useAsync 2 */

/* _WriteParam takes A0 = SysParam ($1F8) and D0 = its 20 bytes, as Apple's glue sets them up. Retro68's
 * WriteParam() is only the trap word, so the registers are set here. */
static void write_param(void)
{
    __asm__ volatile(
        "   lea     0x1F8,%%a0\n"
        "   moveq   #20,%%d0\n"
        "   .short  0xA038\n"               /* _WriteParam */
        ::: "d0", "d1", "d2", "a0", "a1", "cc", "memory");
}

void _start(void)
{
    Byte config = LMGetSPConfig();

    if ((config & 0x0F) != kUseATalk) {
        LMSetSPConfig((config & 0xF0) | kUseATalk);
        write_param();
    }
}
