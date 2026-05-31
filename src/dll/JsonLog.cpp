#include "JsonLog.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace dx12track {

namespace {

// UTF-16 -> UTF-8, with JSON escaping for control chars, ", and \.
void AppendJsonString(std::string& out, const wchar_t* s, size_t max_chars) {
    out.push_back('"');
    if (!s) { out.push_back('"'); return; }
    char utf8[8];
    for (size_t i = 0; i < max_chars && s[i]; ++i) {
        wchar_t c = s[i];
        switch (c) {
            case L'\\': out.append("\\\\"); continue;
            case L'"':  out.append("\\\""); continue;
            case L'\n': out.append("\\n");  continue;
            case L'\r': out.append("\\r");  continue;
            case L'\t': out.append("\\t");  continue;
            default: break;
        }
        if (c < 0x20) {
            char esc[8]; sprintf(esc, "\\u%04x", (unsigned)c);
            out.append(esc); continue;
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, sizeof(utf8),
                                    nullptr, nullptr);
        if (n > 0) out.append(utf8, utf8 + n);
    }
    out.push_back('"');
}

void AppendU64(std::string& out, uint64_t v) {
    char buf[32]; sprintf(buf, "%llu", (unsigned long long)v);
    out.append(buf);
}

void AppendKey(std::string& out, const char* k) {
    out.push_back('"'); out.append(k); out.append("\":");
}

} // namespace

bool JsonLog::Open(const std::wstring& path) {
    file_ = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                       FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    return file_ != INVALID_HANDLE_VALUE;
}

void JsonLog::Close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

void JsonLog::WriteLineLocked(const char* s, size_t len) {
    if (file_ == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file_, s, (DWORD)len, &written, nullptr);
}

void JsonLog::Append(EventKind kind, uint64_t ts_ns,
                     const void* payload, size_t payload_bytes) {
    if (file_ == INVALID_HANDLE_VALUE) return;
    std::string out;
    out.reserve(384);

    auto comma_kv_u64 = [&](const char* k, uint64_t v) {
        out.push_back(','); AppendKey(out, k); AppendU64(out, v);
    };
    auto comma_kv_str = [&](const char* k, const char* v) {
        out.push_back(','); AppendKey(out, k);
        out.push_back('"'); out.append(v); out.push_back('"');
    };

    out.push_back('{');
    AppendKey(out, "event");

    switch (kind) {
        case EventKind::Hello: {
            out.append("\"hello\"");
            comma_kv_u64("ts_ns", ts_ns);
            if (payload_bytes >= sizeof(HelloPayload)) {
                auto* p = static_cast<const HelloPayload*>(payload);
                comma_kv_u64("pid", p->pid);
                comma_kv_u64("protocol", p->protocol_version);
                comma_kv_u64("qpc_freq", p->qpc_frequency);
                out.push_back(','); AppendKey(out, "exe");
                AppendJsonString(out, p->exe_path, kMaxNameChars);
            }
            break;
        }
        case EventKind::Created: {
            out.append("\"created\"");
            comma_kv_u64("ts_ns", ts_ns);
            if (payload_bytes >= sizeof(CreatedPayload)) {
                auto* p = static_cast<const CreatedPayload*>(payload);
                comma_kv_u64("id",       p->id);
                comma_kv_str("type",     ObjectTypeName(p->type));
                comma_kv_str("alloc",    AllocationKindName(p->alloc));
                comma_kv_str("heap",     HeapTypeName(p->heap_type));
                comma_kv_str("dim",      DimensionName(p->dimension));
                comma_kv_u64("format",   p->format);
                comma_kv_u64("size",     p->size_bytes);
                comma_kv_u64("parent_heap_id", p->parent_heap_id);
                out.push_back(','); AppendKey(out, "name");
                AppendJsonString(out, p->name, kMaxNameChars);
            }
            break;
        }
        case EventKind::Renamed: {
            out.append("\"renamed\"");
            comma_kv_u64("ts_ns", ts_ns);
            if (payload_bytes >= sizeof(RenamedPayload)) {
                auto* p = static_cast<const RenamedPayload*>(payload);
                comma_kv_u64("id", p->id);
                out.push_back(','); AppendKey(out, "name");
                AppendJsonString(out, p->name, kMaxNameChars);
            }
            break;
        }
        case EventKind::Destroyed: {
            out.append("\"destroyed\"");
            comma_kv_u64("ts_ns", ts_ns);
            if (payload_bytes >= sizeof(DestroyedPayload)) {
                auto* p = static_cast<const DestroyedPayload*>(payload);
                comma_kv_u64("id", p->id);
            }
            break;
        }
        case EventKind::Goodbye: {
            out.append("\"goodbye\"");
            comma_kv_u64("ts_ns", ts_ns);
            if (payload_bytes >= sizeof(GoodbyePayload)) {
                auto* p = static_cast<const GoodbyePayload*>(payload);
                comma_kv_u64("exit_code", p->exit_code);
            }
            break;
        }
    }

    out.push_back('}');
    out.push_back('\n');

    std::lock_guard<std::mutex> lock(mu_);
    WriteLineLocked(out.data(), out.size());
}

JsonLog& GlobalLog() {
    static JsonLog g;
    return g;
}

} // namespace dx12track
