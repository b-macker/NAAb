// codemap fixture: a translation unit the index parses but no fixture binary
// links -- its functions are "not in the build", which coverage must not report
// as untested.
#include "fixture.h"

int never_linked(int x) { return x - 1; }
