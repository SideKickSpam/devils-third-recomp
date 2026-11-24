// Frame-rate independence for the game's UnrealScript: accumulating operators at frame-tied sites are scaled to the
// frame time (frametie.cpp).
#pragma once

namespace frametie
{
// Once per UGameEngine::Tick, on the Game Thread, with its DeltaSeconds.
void Tick(float deltaSeconds);
}
