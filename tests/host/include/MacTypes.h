/* Host-test stand-ins for the few Mac types the portable talkip code uses. */
#ifndef HOST_MACTYPES_H
#define HOST_MACTYPES_H
#include <stdbool.h>
typedef short OSErr;
typedef unsigned char Boolean;
typedef char *Ptr;
#define noErr 0
#define paramErr (-50)
#define fBsyErr (-47)
#endif
