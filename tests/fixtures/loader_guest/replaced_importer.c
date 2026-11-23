/* Imports g6replaced.dll the way a side DLL imports a DLL we replace. Without a replacement the
 * loader maps g6replaced.dll as a guest module; with one, the import binds through the import
 * registry like any other non-guest DLL. No C runtime and no entry point. */

#include "loader_guest_api.h"

__declspec(dllimport) int g6replaced_value(void);

__declspec(dllexport) int g6_importer_call(void) {
    return g6replaced_value();
}
