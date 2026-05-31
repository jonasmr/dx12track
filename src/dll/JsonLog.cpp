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
                if (p->frame_count) {
                    out.push_back(','); AppendKey(out, "stack");
                    out.push_back('[');
                    char hex[24];
                    size_t n = p->frame_count;
                    if (n > kMaxCallstackFrames) n = kMaxCallstackFrames;
                    for (size_t i = 0; i < n; ++i) {
                        if (i) out.push_back(',');
                        sprintf(hex, "\"0x%llx\"",
                                (unsigned long long)p->frames[i]);
                        out.append(hex);
                    }
                    out.push_back(']');
                }
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
        case EventKind::ModuleLoaded: {
            out.append("\"module_loaded\"");
            comma_kv_u64("ts_ns", ts_ns);
            if (payload_bytes >= sizeof(ModuleLoadedPayload)) {
                auto* p = static_cast<const ModuleLoadedPayload*>(payload);
                char hex[24];
                out.push_back(','); AppendKey(out, "base");
                sprintf(hex, "\"0x%llx\"", (unsigned long long)p->base);
                out.append(hex);
                comma_kv_u64("size", p->size);
                comma_kv_u64("timestamp", p->timestamp);
                comma_kv_u64("pdb_age", p->pdb_age);
                // PDB GUID as the canonical 8-4-4-4-12 hex form expected by
                // symsrv (lowercase, no dashes is also accepted; we emit with
                // dashes for readability).
                {
                    const uint8_t* g = p->pdb_guid;
                    char buf[64];
                    sprintf(buf,
                        "\"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\"",
                        g[3], g[2], g[1], g[0],   // Data1 little-endian
                        g[5], g[4],               // Data2 little-endian
                        g[7], g[6],               // Data3 little-endian
                        g[8], g[9],
                        g[10], g[11], g[12], g[13], g[14], g[15]);
                    out.push_back(','); AppendKey(out, "pdb_guid");
                    out.append(buf);
                }
                out.push_back(','); AppendKey(out, "name");
                AppendJsonString(out, p->name, kMaxNameChars);
                out.push_back(','); AppendKey(out, "pdb_name");
                AppendJsonString(out, p->pdb_name, kMaxNameChars);
            }
            break;
        }
        case EventKind::ModuleUnloaded: {
            out.append("\"module_unloaded\"");
            comma_kv_u64("ts_ns", ts_ns);
            if (payload_bytes >= sizeof(ModuleUnloadedPayload)) {
                auto* p = static_cast<const ModuleUnloadedPayload*>(payload);
                char hex[24];
                sprintf(hex, "\"0x%llx\"", (unsigned long long)p->base);
                out.push_back(','); AppendKey(out, "base");
                out.append(hex);
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
