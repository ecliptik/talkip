/*
 * trapcatch.c - the TalkIP Driver's crash readout (IPP_TRAPCATCH, on by default).
 *
 * A 512Ke has no debugger: a crash is a bomb box with only its name. With this built in, the INIT also takes the
 * bus error, address error, illegal instruction and Line-F vectors. On one of those it draws, in hex at the
 * bottom of the screen: the vector, the PC, the PC as an offset into the driver, SR, the driver's base, the SP
 * at the fault, the access address and instruction (bus and address errors), D0-D7, A0-A6 and the stack. Then
 * it goes on to the system's own handler, so the bomb box comes up as before, and a screenshot reads the lot.
 * It masks interrupts and makes no Toolbox calls: the heap may be gone.
 */
#include <stdint.h>
#include <Memory.h>

extern char _stext[];

#define LM_SCRN_BASE    (*(uint8_t **)0x824)
#define ROW_BYTES       64
#define SCREEN_ROWS     342
#define LINES           8

uint16_t ipp_trap_vec;
uint32_t ipp_trap_old[4];               /* $08, $0C, $10, $2C */
static uint8_t busy;

/* 5x7 glyphs, a row per byte, bits 4..0 left to right */
static const char glyph_chars[] = "0123456789ABCDEFPSRVI=-";
static const uint8_t glyphs[][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
};

static void draw_line(int line, const char *s)
{
    uint8_t *row = LM_SCRN_BASE + (long)(SCREEN_ROWS - (LINES - line) * 8) * ROW_BYTES;
    int col, y, g;

    for (col = 0; col < ROW_BYTES; col++) {
        const uint8_t *bits = NULL;
        if (*s) {
            for (g = 0; glyph_chars[g]; g++)
                if (glyph_chars[g] == *s)
                    bits = glyphs[g];
            s++;
        }
        row[col] = 0;
        for (y = 0; y < 7; y++)
            row[(y + 1) * ROW_BYTES + col] = bits ? (uint8_t)(bits[y] << 2) : 0;
    }
}

static char *put_hex(char *p, uint32_t v, int digits)
{
    while (digits--)
        *p++ = "0123456789ABCDEF"[(v >> (digits * 4)) & 15];
    return p;
}

static char *put_label(char *p, const char *label, uint32_t v, int digits)
{
    while (*label)
        *p++ = *label++;
    p = put_hex(p, v, digits);
    *p++ = ' ';
    return p;
}

/* regs: D0-D7, A0-A6 as the entry saved them; the exception frame follows */
void ipp_trap_show(uint32_t *regs)
{
    uint8_t *frame = (uint8_t *)(regs + 15);
    Boolean group0 = ipp_trap_vec == 0x08 || ipp_trap_vec == 0x0C;     /* 14-byte frame */
    uint8_t *sr_pc = group0 ? frame + 8 : frame;
    uint16_t sr = *(uint16_t *)sr_pc;
    uint32_t pc = *(uint32_t *)(sr_pc + 2);
    uint32_t *sp = (uint32_t *)(sr_pc + 6);
    uint32_t base = (uint32_t)_stext;
    char buf[ROW_BYTES + 1], *p;
    int line, i, n = 0;

    if (busy)
        return;
    busy = 1;
    p = buf;
    p = put_label(p, "V=", ipp_trap_vec, 2);
    p = put_label(p, "PC=", pc, 8);
    p = put_label(p, "PC-B=", pc - base, 8);
    p = put_label(p, "SR=", sr, 4);
    *p = 0;
    draw_line(0, buf);
    p = buf;
    p = put_label(p, "B=", base, 8);
    p = put_label(p, "SP=", (uint32_t)sp, 8);
    if (group0) {
        p = put_label(p, "A=", *(uint32_t *)(frame + 2), 8);
        p = put_label(p, "I=", *(uint16_t *)(frame + 6), 4);
    }
    *p = 0;
    draw_line(1, buf);
    /* D0-D7 A0-A6, then the stack: 7 longwords a line */
    for (line = 2; line < LINES; line++) {
        p = buf;
        for (i = 0; i < 7; i++, n++) {
            p = put_hex(p, n < 15 ? regs[n] : sp[n - 15], 8);
            *p++ = ' ';
        }
        *p = 0;
        draw_line(line, buf);
    }
    busy = 0;
}

void ipp_catch_08(void), ipp_catch_0C(void), ipp_catch_10(void), ipp_catch_2C(void);

#define CATCH(v, off) \
    "   .globl  ipp_catch_" #v "\n" \
    "ipp_catch_" #v ":\n" \
    "   ori.w   #0x0700,%sr\n" \
    "   move.w  #0x" #v ",ipp_trap_vec\n" \
    "   movem.l %d0-%d7/%a0-%a6,-(%sp)\n" \
    "   move.l  %sp,-(%sp)\n" \
    "   jsr     ipp_trap_show\n" \
    "   addq.l  #4,%sp\n" \
    "   movem.l (%sp)+,%d0-%d7/%a0-%a6\n" \
    "   move.l  ipp_trap_old+" #off ",-(%sp)\n" \
    "   rts\n"

__asm__(
    "   .text\n"
    "   .even\n"
    CATCH(08, 0)
    CATCH(0C, 4)
    CATCH(10, 8)
    CATCH(2C, 12)
);

void ipp_trapcatch_install(void)
{
    static const uint16_t vectors[4] = { 0x08, 0x0C, 0x10, 0x2C };
    void (*const entries[4])(void) = { ipp_catch_08, ipp_catch_0C, ipp_catch_10, ipp_catch_2C };
    int i;

    for (i = 0; i < 4; i++) {
        ipp_trap_old[i] = *(uint32_t *)(uintptr_t)vectors[i];
        *(uint32_t *)(uintptr_t)vectors[i] = (uint32_t)entries[i];
    }
}
