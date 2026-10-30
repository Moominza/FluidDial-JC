// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Compile-time policy check for the BLE transport (Instruction 010): the ordinary firmware's BLE link has NO
// command transmit function - only the read-only query path - while the full-control test build has the one
// that sits behind the shared command policy. Built twice by tools/run_host_tests.py.

#include <stdio.h>
#include <type_traits>
#include <utility>

#include "../../src/jc8048w550/JcBleUart.hpp"

using namespace jc8048w550;

template <typename T>
auto has_command_path(int) -> decltype(std::declval<T&>().send_command_bytes("", size_t(0)), std::true_type());
template <typename T>
std::false_type has_command_path(...);

#if JC_FULL_CONTROL_TEST
static_assert(decltype(has_command_path<JcBleUart>(0))::value, "the full-control build must have the policy-checked BLE command path");
#else
static_assert(!decltype(has_command_path<JcBleUart>(0))::value, "the ordinary build must have NO BLE command transmit function");
#endif

int main() {
    JcBleUart link;
    // Not started: nothing can be queued, in either build.
    const bool q = link.send_query_line("$G");
    printf("ble policy (%s): query on a stopped link accepted=%d\n", JC_FULL_CONTROL_TEST ? "full-control" : "ordinary", q ? 1 : 0);
    return q ? 1 : 0;
}
