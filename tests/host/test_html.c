/* Host test for html.c: feed a page (file or stdin) in small chunks, print the wrapped lines with styles.
 *   make -C tests/host test_html && curl -s http://info.cern.ch/ | tests/host/test_html */
#include <stdio.h>
#include <string.h>
#include "html.h"

static short face;
short CharWidth(short c) { return face ? 7 : 6; }   /* a fixed-pitch stand-in: 6 px, 7 bold */
void TextFace(short f) { face = f; }

int main(int argc, char **argv)
{
    static Doc d;
    unsigned char buf[37];          /* odd chunk size: tags and UTF-8 split across feeds */
    size_t n;
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    short i;

    if (!f || !doc_init(&d))
        return 2;
    doc_begin(&d, argc > 2, 480);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        doc_feed(&d, buf, (unsigned short)n);
    doc_end(&d);
    printf("title: %s\nlen %ld, lines %d, links %d%s\n", d.title, d.len, d.nlines, d.nlinks,
           d.truncated ? ", TRUNCATED" : "");
    for (i = 0; i < d.nlines && i < 60; i++) {
        DocLine *l = &d.lines[i];
        long k;
        printf("%3d%c%c|", i, l->bold ? 'B' : ' ', l->link ? 'L' : ' ');
        for (k = l->start; k < l->start + l->len; k++) {
            unsigned char c = (unsigned char)d.text[k];
            if (c == DOC_BOLD_ON) printf("{b}");
            else if (c == DOC_BOLD_OFF) printf("{/b}");
            else if (c == DOC_LINK_ON) { printf("{a%d}", (unsigned char)d.text[++k]); }
            else if (c == DOC_LINK_OFF) printf("{/a}");
            else if (c == DOC_RULE) printf("------");
            else if (c >= 0x80) printf("<%02X>", c);
            else putchar(c);
        }
        printf("|\n");
    }
    for (i = 0; i < d.nlinks && i < 8; i++)
        printf("link %d: %s\n", i, doc_link(&d, i));
    return 0;
}
