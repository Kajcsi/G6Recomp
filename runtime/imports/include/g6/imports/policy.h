#pragma once

// The import policy the game runs under (docs/side-dlls.md, "Default import policy", and the
// per-DLL tables in docs/hle-status.md): what each import of a guest module binds to when it
// isn't another guest module. Deny by default: a DLL or function no rule names is stubbed.

#include "g6/loader/import_registry.h"

namespace g6::imports {

// The registry for the pinned 1.0.2.1 build. The Loader adds its own loader-function HLE on top.
loader::ImportRegistry default_game_policy();

} // namespace g6::imports
