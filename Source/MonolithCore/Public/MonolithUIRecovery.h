#pragma once

#include "CoreMinimal.h"

// The editor's deferred UI owner may service HTTP between modal ticks only
// outside registry dispatch. Recovery mode restricts domain actions to UI
// observation and queued UI recovery; it never authorizes general reentrancy.
MONOLITHCORE_API bool IsMonolithActionExecuting();
MONOLITHCORE_API bool SetMonolithUIRecoveryOnly(bool bEnabled);
