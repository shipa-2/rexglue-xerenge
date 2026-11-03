/**
 * @file        ui/uwp_game_setup.h
 * @brief       UWP: the app's own first-run step, for a game folder that is
 *              still empty - the runtime knows nothing of how the game arrives
 */

#pragma once

#include <filesystem>

namespace rex::ui {

// Called on the UI thread, its CoreWindow activated, before the app is made,
// when neither the package nor LocalState has the game; it is to fill
// game_dir (LocalState\game). Returns false to quit instead.
using UwpGameSetupFn = bool (*)(const std::filesystem::path& game_dir);

// Registered from a static initializer of the app.
void SetUwpGameSetup(UwpGameSetupFn setup);

}  // namespace rex::ui
