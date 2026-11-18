// SPDX-License-Identifier: GPL-3.0-or-later
#include "ControllerHaptics.h"

MlControllerHapticsCallback MlHapticsCallback = NULL;

// Defined only by common-c-haptics.patch in ControlStream.c. Taking its address
// here makes any build that calls the setter fail to link if the networking code
// was rebuilt without the patch, instead of shipping with haptics half wired.
extern const int MlHapticsPatchApplied;
const int* volatile MlHapticsPatchMarker = &MlHapticsPatchApplied;

void LiSetControllerHapticsCallback(MlControllerHapticsCallback callback) {
    MlHapticsCallback = callback;
}
