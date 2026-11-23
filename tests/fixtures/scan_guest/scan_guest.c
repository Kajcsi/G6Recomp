/* The compiled part of the scan test guest (docs/scan.md): thread-local storage the way the
 * compiler accesses it, through the module's TLS index. Written for these tests; the DLL is
 * only ever read as a file. */

static __declspec(thread) int scan_tls_counter = 1;

int scan_tls_bump(void) { /* NOLINT(misc-use-internal-linkage): exported through the .def */
    return ++scan_tls_counter;
}
