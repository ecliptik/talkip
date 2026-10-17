/* TalkIP Driver: the M8 prototype INIT. Loaded into the system heap (sysheap), locked; it detaches itself. */
#include "Retro68.r"

type 'INIT' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (128, sysheap, locked) {
    dontBreakAtEntry, $$read("TalkIPDriver.flt");
};

/* System heap the INIT needs at boot (INIT31 grows the system heap by this much before loading it) */
type 'sysz' {
    longint;
};

resource 'sysz' (0) {
    SYSZ_BYTES
};
