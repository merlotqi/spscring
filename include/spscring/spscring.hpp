#pragma once

// spscring — single-producer/single-consumer lock-free ring buffers.
//
// Header-only, C++17, no third-party dependencies, no allocations.
//   - fixed ring:   constant-size frames, one CAS per reserve
//   - varlen ring:  variable-length messages with a self-describing slot header
//
// Designed for shared-memory IPC: the control block is a pinned standard-layout
// ABI and the wait primitives map to futex (Linux), WaitOnAddress (Win32), and
// os_sync_wait_on_address (Darwin).

#include <spscring/atomic_backoff.hpp>
#include <spscring/atomic_wait.hpp>
#include <spscring/control_block.hpp>
#include <spscring/fixed_reader.hpp>
#include <spscring/fixed_writer.hpp>
#include <spscring/message_meta.hpp>
#include <spscring/platform.hpp>
#include <spscring/reserve_result.hpp>
#include <spscring/ring_view.hpp>
#include <spscring/varlen_header.hpp>
#include <spscring/varlen_reader.hpp>
#include <spscring/varlen_writer.hpp>
