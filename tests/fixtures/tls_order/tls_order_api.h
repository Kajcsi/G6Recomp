#pragma once

/* Shared by tls_order.dll and test_tls_order. */

/* The first word of tls_order.dll's TLS template, so a block filled from it is recognizable. */
enum { G6_TLS_ORDER_MAGIC = 0x6F7A1D5B };
