/*
 * hello.c - M8: the smallest program to send to the 512Ke with Retro68's LaunchAPPL over TCP (through the .ipp
 * driver). LaunchAPPL's server returns whatever the program writes to the file "out" in its folder; no console
 * library (that would be 760 KB).
 */
#include <string.h>
#include <stdio.h>
#include <Files.h>
#include <Memory.h>

int main(void)
{
    char buf[160];
    long n;
    short ref;

    sprintf(buf, "Hello from the Macintosh 512Ke, over MacIP and the TalkIP Driver. FreeMem %ld bytes.\r", FreeMem());
    n = (long)strlen(buf);
    if (FSOpen("\pout", 0, &ref) == noErr) {
        FSWrite(ref, &n, buf);
        FSClose(ref);
    }
    return 0;
}
