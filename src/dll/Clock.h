#pragma once

#include <cstdint>

namespace dx12track {

// The single time base for every ts_ns the DLL emits (pipe headers and the
// JSONL log): ts_ns = (QPC - QpcStart()) * 1e9 / QpcFrequency().
//
// QpcStart() is sent in the Hello event so consumers can put other QPC-stamped
// data (e.g. the launcher's ETW events, which carry raw QPC timestamps) on the
// same timeline. InitClock() is idempotent; DllMain calls it first thing, and
// every accessor initializes lazily in case something runs earlier.
void     InitClock();
uint64_t QpcStart();
uint64_t QpcFrequency();
uint64_t NowNs();

} // namespace dx12track
