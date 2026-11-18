#pragma once

// DualSense waveform ("HD") haptics from Vibeshine hosts, played over Bluetooth
// on macOS. The names match Nonary's Moonlight fork so the wiring reads the same.

#include <cstdint>

#include <SDL.h>

namespace DualSenseHaptics {

// False when ML_DS5_HAPTICS=0: the session then advertises nothing and attach() refuses.
bool enabled();

// Starts a player for a PS5 controller connected over Bluetooth. Returns true if
// the controller can play waveforms, so the caller can advertise LI_CCAP_HAPTICS_PCM.
bool attach(int slot, SDL_GameController* controller);

// Stops the player and waits for its worker. Call before closing the controller.
void detach(int slot);

// True while a waveform is playing; ordinary rumble is muted meanwhile.
bool playing(int slot);

// Network callback (MlControllerHapticsCallback): copies the packet and returns.
void receive(uint16_t controllerNumber, uint32_t sequence, const uint8_t* pcm, uint16_t frames);

}  // namespace DualSenseHaptics
