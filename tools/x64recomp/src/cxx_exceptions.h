#pragma once

// The MSVC C++ exception facts of a module (docs/hooks.md, "C++ exception functions"): which
// functions __CxxFrameHandler3 or __CxxFrameHandler4 covers, their exception tables, and where
// their catch blocks resume, plus the functions whose only handler checks the stack cookie. The
// hook engine uses it to check patch windows in these functions, and the scan reports it.

#include <cstdint>
#include <string>
#include <vector>

#include "function_discovery.h"
#include "g6/pe/cxx_eh.h"
#include "g6/pe/image.h"

namespace g6::recomp {

struct CxxFunction {
    uint32_t start = 0;
    uint32_t tables = 0;     // the FuncInfo's RVA
    bool gs_wrapper = false; // the handler is __GSHandlerCheck_EH or _EH4, which calls the real one
    // A catch or cleanup funclet: code that runs for another function's frame, in its states.
    bool funclet = false;
    // Every catch of the function's tables resumes somewhere a source names (see
    // CxxExceptionMap). Funclets never count as verifiable.
    bool verifiable = false;
    pe::CxxFuncInfo info;
    std::vector<uint32_t> continuations; // where catches resume inside this function, sorted
};

// A function whose handler is a C++ handler but whose tables don't read.
struct CxxTableError {
    uint32_t start = 0;
    std::string message;
};

struct CxxExceptionStats {
    size_t handlers = 0; // functions (primary .pdata entries) with a language handler
    size_t fh3 = 0;
    size_t fh4 = 0;
    size_t by_magic = 0; // FH3 found by the magic number alone (a static C runtime's handler)
    size_t gs_wrapped = 0;
    size_t funclets = 0;
    size_t with_catches = 0;        // non-funclet functions with at least one catch
    size_t unverifiable = 0;        // of those, the ones with a catch no source accounts for
    size_t unreadable = 0;          // a C++ handler, but tables that don't read
    size_t gs_only = 0;             // the handler only checks the stack cookie (__GSHandlerCheck)
    size_t gs_handlers = 0;         // the distinct handlers that do only that
    size_t other_handlers = 0;      // any other handler, C scope tables included
    size_t stray_continuations = 0; // recorded or returned, but not in the guard table
};

struct CxxExceptionMap {
    std::vector<CxxFunction> functions; // sorted by start
    std::vector<CxxTableError> errors;  // sorted by start
    // Catch continuations by source, each sorted and without duplicates. The guard table lists
    // every place an exception may resume in the module, C __except blocks included, so when
    // the module has one, every catch counts as accounted for.
    bool guard_table = false;            // the module has a CFG EH continuation table
    std::vector<uint32_t> guard;         // that table's entries
    std::vector<uint32_t> recorded;      // the addresses FH4 records for its catches
    std::vector<uint32_t> returned;      // the addresses catch funclets return (`lea` into the
                                         // function that owns the catch)
    std::vector<uint32_t> continuations; // all of the above
    // The functions whose only handler checks the stack cookie, as the C runtime's
    // __GSHandlerCheck does for a function with a /GS buffer and no exception handling. Such a
    // handler never catches and runs no cleanups. Sorted.
    std::vector<uint32_t> gs_only;
    CxxExceptionStats stats;

    const CxxFunction* find(uint32_t start) const;
    bool is_gs_only(uint32_t start) const;
};

// Reads the C++ exception tables of every function `discovery` found. A handler counts as C++
// when it is __CxxFrameHandler3 or __CxxFrameHandler4 (imported or exported by name), a GS
// wrapper that calls one of them, or, failing those, when its data leads to an FH3 FuncInfo's
// magic number, which is how a static C runtime's handler shows up. A handler that is none of
// those counts as a cookie check when it behaves like __GSHandlerCheck: its code makes one call,
// to code that reads the module's security cookie, and then returns ExceptionContinueSearch.
CxxExceptionMap map_cxx_exceptions(const pe::Image& image, const DiscoveryResult& discovery);

} // namespace g6::recomp
