// OM1: iOS archive-link stub. AppleEmbeddedStubs.cpp answers every IOCtlSrc
// method except SetSpindleSpeed (declared in CDVDdiscReader.h); without it
// any member referencing the spindle call leaves an undefined symbol at the
// app link. Never runs on the offline path (no disc on iOS).
// OM1-private (lives in the OM1 scratch ARMSX2 tree; never in the fork).
#include "CDVD/CDVDdiscReader.h"

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
	(void)restore_defaults;
}
