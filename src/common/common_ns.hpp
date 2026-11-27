#pragma once
// Compile-time namespace isolation for shared low-level OS/socket/clock/CRC primitives.
// Each protocol target compiles src/common/ under its own namespace so static libraries are
// 100% self-contained AND both liblanvid_core and libcrosslink_core can be linked together
// in the same executable with zero duplicate symbols or ODR violations:
//   - lanvid_core    : -DNET_COMMON_NS=lv_sys -DNET_COMMON_IMPORT_NS=lv
//   - crosslink_core : -DNET_COMMON_NS=cl_sys -DNET_COMMON_IMPORT_NS=cl

#ifndef NET_COMMON_NS
#define NET_COMMON_NS lv_sys
#endif

namespace NET_COMMON_NS {}

#if defined(NET_COMMON_IMPORT_NS)
namespace NET_COMMON_IMPORT_NS {
using namespace NET_COMMON_NS;
}  // namespace NET_COMMON_IMPORT_NS
#endif
