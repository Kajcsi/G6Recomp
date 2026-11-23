/* A real DLL in the loader's search path that the replaced-DLL tests mark as replaced: the
 * loader must find our implementation instead and never map this file. No C runtime and no
 * entry point, so mapping it would be cheap and visible only through this export's value. */

#include "loader_guest_api.h"

__declspec(dllexport) int g6replaced_value(void) {
    return G6_REPLACED_ORIGINAL;
}
