// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vibeshine waveform ("HD") controller haptics, wire format version 1.
//
// The constants and the packet validator come from Nonary's moonlight-common-c
// fork (src/ControllerHaptics.h, GPL-3.0-or-later), which defines this protocol.
// This fork reaches the packet through a setter instead of a new field in
// CONNECTION_LISTENER_CALLBACKS, so upstream's Limelight.h stays untouched.
//
// The host sends packet type 0x5601 on control channel 0x08, unreliable, only when
// the client advertises ML_FF_HAPTICS_PCM in its SDP feature flags and
// LI_CCAP_HAPTICS_PCM in the controller arrival event. All multibyte fields and
// samples are little-endian.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ML_HAPTICS_PACKET_TYPE 0x5601
#define ML_HAPTICS_CHANNEL 0x08
#define ML_HAPTICS_HEADER_SIZE 12
#define ML_HAPTICS_MAX_FRAMES 240
#define ML_HAPTICS_FRAME_BYTES 4
#define ML_HAPTICS_MAX_PAYLOAD (ML_HAPTICS_HEADER_SIZE + ML_HAPTICS_MAX_FRAMES * ML_HAPTICS_FRAME_BYTES)
#define LI_CCAP_HAPTICS_PCM 0x8000
#define ML_FF_HAPTICS_PCM 0x04

static inline uint16_t MlHapticsRead16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t MlHapticsRead32(const uint8_t* p) {
    return (uint32_t)MlHapticsRead16(p) | ((uint32_t)MlHapticsRead16(p + 2) << 16);
}

// Payload: version=1, format=1 (48 kHz S16LE stereo), controller u16, sequence u32,
// frame count u16, reserved u16=0, then interleaved left/right actuator samples.
static inline int MlHapticsValidate(const uint8_t* p, size_t size) {
    if (!p || size < ML_HAPTICS_HEADER_SIZE || p[0] != 1 || p[1] != 1 ||
        MlHapticsRead16(p + 2) >= 16 || MlHapticsRead16(p + 10) != 0) {
        return 0;
    }
    uint16_t frames = MlHapticsRead16(p + 8);
    return frames > 0 && frames <= ML_HAPTICS_MAX_FRAMES &&
        size == ML_HAPTICS_HEADER_SIZE + (size_t)frames * ML_HAPTICS_FRAME_BYTES;
}

// Called on the control stream's receive thread for each valid packet. pcm points
// at frames * 2 interleaved S16LE samples and is only valid during the call, so
// copy it and return quickly.
typedef void (*MlControllerHapticsCallback)(uint16_t controllerNumber, uint32_t sequence,
                                            const uint8_t* pcm, uint16_t frames);

// Set before LiStartConnection(). NULL (the default) leaves the stream exactly as
// upstream: no feature flag is advertised and haptics packets are ignored.
void LiSetControllerHapticsCallback(MlControllerHapticsCallback callback);

// Read by the patched SdpGenerator.c and ControlStream.c.
extern MlControllerHapticsCallback MlHapticsCallback;

#ifdef __cplusplus
}
#endif
