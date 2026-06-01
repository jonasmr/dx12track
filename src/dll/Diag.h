#pragma once

namespace dx12track {

// Controlled by DX12TRACK_VERBOSE env var (read in DllMain). When false, DiagF
// is a near-no-op (one branch + return) so the verbose hooks don't pay any
// real cost on release builds with the flag off.
extern bool g_verbose;

// printf-style diagnostic emit. Truncates at kMaxDiagnosticChars-1. Format
// the same hex/u64 fields you'd put in a log line. Routes to both the pipe
// and the JSONL via the existing senders.
void DiagF(const char* fmt, ...);

} // namespace dx12track
