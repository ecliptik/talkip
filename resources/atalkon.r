/* AppleTalk On: an INIT that turns AppleTalk on at startup. Runs once and is released (not sysheap, not locked). */
#include "Retro68.r"

type 'INIT' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (128) {
    dontBreakAtEntry, $$read("AppleTalkOn.flt");
};
