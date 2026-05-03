#pragma once

namespace PrimedGun {
struct SharedState;
}

namespace PrimedGun::Hook::GameTimingHooks {

bool Install();
void SuppressLockCameraPitchForLogicTick();
void Poll(SharedState* state);
void Shutdown();

} // namespace PrimedGun::Hook::GameTimingHooks
