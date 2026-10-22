// SPDX-License-Identifier: GPL-2.0-or-later
// bbport 0.6's hooks in the game's code (deadinside28/bloodborne_pc 8185b88), installed with the
// image (bbgpu_patch_image) on every memory model: before the game's code runs and before the
// recompiled library is checked against it (hooked functions stay with the translator).
#pragma once

#include <cstdint>

namespace BbGameHooks {
void PatchImage(unsigned char* image, std::uint64_t size);
/// The effects switched live (all but screen-space reflections) follow the settings now.
void ApplyGameEffects();
/// Whether effects are switched live (else they are patches at start and need a restart).
bool HotEffects();
/// The output's aspect ratio: the camera, the HUD's markers and the UI follow it (16:9: the game's).
void SetDisplayAspect(float aspect);
/// Native mouse look: the turn is added at the camera's next update; false when the camera does
/// not take turns now (lock-on, menus) or the hook is not in: drive the right stick then.
bool CameraLookAvailable();
bool CameraLookAdd(float pitch, float yaw);
/// The free camera (the Restore Debug Camera patch) is on.
bool FreeCameraActive();
} // namespace BbGameHooks
