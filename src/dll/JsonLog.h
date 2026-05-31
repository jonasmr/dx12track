#pragma once

#include <mutex>
#include <string>
#include <windows.h>

#include "EventTypes.h"

namespace dx12track {

class JsonLog {
public:
    bool Open(const std::wstring& path);
    void Close();

    // Append one event. Caller passes the same payload as PipeClient::Send.
    void Append(EventKind kind, uint64_t ts_ns,
                const void* payload, size_t payload_bytes);

private:
    void WriteLineLocked(const char* s, size_t len);

    HANDLE     file_ = INVALID_HANDLE_VALUE;
    std::mutex mu_;
};

JsonLog& GlobalLog();

} // namespace dx12track
