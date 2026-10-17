/* bytes.h - network byte order, byte by byte (no alignment assumptions: the 68000 faults on odd words). */
#ifndef TALKIP_BYTES_H
#define TALKIP_BYTES_H

static inline unsigned short get16(const unsigned char *p)
{
    return (unsigned short)((p[0] << 8) | p[1]);
}

static inline unsigned long get32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

static inline void put16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static inline void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

#endif
