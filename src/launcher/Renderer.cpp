#include "Renderer.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>

namespace dx12track {

namespace {

constexpr WORD kAttrNormal  = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
constexpr WORD kAttrHeader  = FOREGROUND_INTENSITY | FOREGROUND_RED |
                              FOREGROUND_GREEN | FOREGROUND_BLUE;
constexpr WORD kAttrAccent  = FOREGROUND_INTENSITY | FOREGROUND_GREEN;
constexpr WORD kAttrWarn    = FOREGROUND_INTENSITY | FOREGROUND_RED;

void FormatBytes(uint64_t bytes, wchar_t* out, size_t cap) {
    if (bytes >= (uint64_t)1 << 30)
        swprintf(out, cap, L"%6.2f GB", (double)bytes / (double)((uint64_t)1 << 30));
    else if (bytes >= (uint64_t)1 << 20)
        swprintf(out, cap, L"%6.2f MB", (double)bytes / (double)((uint64_t)1 << 20));
    else if (bytes >= (uint64_t)1 << 10)
        swprintf(out, cap, L"%6.2f KB", (double)bytes / (double)((uint64_t)1 << 10));
    else
        swprintf(out, cap, L"%5llu  B", (unsigned long long)bytes);
}

void FormatNs(uint64_t ns, wchar_t* out, size_t cap) {
    uint64_t total_s = ns / 1'000'000'000ull;
    uint64_t h = total_s / 3600;
    uint64_t m = (total_s % 3600) / 60;
    uint64_t s = total_s % 60;
    swprintf(out, cap, L"%llu:%02llu:%02llu",
        (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
}

// True when --etw was requested and the session came up (live or finished).
bool EtwOn(const Model::Snapshot& snap) {
    return snap.etw.enabled && snap.etw.status.empty();
}

} // namespace

Renderer::Renderer() = default;

bool Renderer::Init() {
    stdout_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdout_ == INVALID_HANDLE_VALUE) return false;

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(stdout_, &info)) {
        cols_ = info.dwSize.X;
        rows_ = info.dwSize.Y;
    }
    Resize(cols_, rows_);
    return true;
}

void Renderer::Resize(SHORT cols, SHORT rows) {
    cols_ = cols;
    rows_ = rows;
    buffer_.assign(static_cast<size_t>(cols) * rows, CHAR_INFO{});
    Clear();
}

void Renderer::Clear(WORD attr) {
    for (auto& ci : buffer_) {
        ci.Char.UnicodeChar = L' ';
        ci.Attributes = attr;
    }
}

void Renderer::Put(SHORT x, SHORT y, const wchar_t* s, WORD attr) {
    if (y < 0 || y >= rows_) return;
    SHORT col = x;
    for (; *s && col < cols_; ++s, ++col) {
        if (col < 0) continue;
        CHAR_INFO& ci = buffer_[(size_t)y * cols_ + col];
        ci.Char.UnicodeChar = *s;
        ci.Attributes = attr;
    }
}

void Renderer::PutF(SHORT x, SHORT y, WORD attr, const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap; va_start(ap, fmt);
    vswprintf(buf, 512, fmt, ap);
    va_end(ap);
    Put(x, y, buf, attr);
}

// Two status lines for --etw. Returns the next free row.
SHORT Renderer::RenderEtwStatus(SHORT row, const Model::Snapshot& snap) {
    const EtwSnapshot& e = snap.etw;
    using CK = etw::CounterKind;
    auto ctr = [&](CK k) { return e.counter[static_cast<size_t>(k)]; };
    auto seen = [&](CK k) { return e.counter_seen[static_cast<size_t>(k)]; };

    if (!e.status.empty()) {
        PutF(0, row++, kAttrWarn, L"ETW  off: %ls", e.status.c_str());
        return row;
    }

    if (!seen(CK::LocalResident) && !seen(CK::NonLocalResident)) {
        PutF(0, row++, kAttrNormal, L"ETW  %ls; waiting for memory counters   pgIn %llu  pgOut %llu",
            e.running ? L"session running" : L"session stopped",
            (unsigned long long)e.page_ins, (unsigned long long)e.page_outs);
    } else {
        wchar_t vres[16], vbud[16], sres[16], dem[16];
        FormatBytes(ctr(CK::LocalResident), vres, 16);
        FormatBytes(ctr(CK::LocalBudget), vbud, 16);
        FormatBytes(ctr(CK::NonLocalResident), sres, 16);
        const uint64_t demoted = ctr(CK::DemotedMin) + ctr(CK::DemotedLow) +
                                 ctr(CK::DemotedNormal) + ctr(CK::DemotedHigh) +
                                 ctr(CK::DemotedMax);
        FormatBytes(demoted, dem, 16);
        PutF(0, row++, kAttrNormal,
            L"ETW  VRAM resident %ls / budget %ls   sys resident %ls   demoted %ls   pgIn %llu  pgOut %llu",
            vres, vbud, sres, dem,
            (unsigned long long)e.page_ins, (unsigned long long)e.page_outs);
    }

    // untracked ~= what the kernel says is resident minus what dx12track can
    // account for: heap objects + committed resources (placed resources live
    // inside heaps; reserved ones have no backing) + ETW driver sizes.
    uint64_t tracked = 0;
    for (size_t h = 0; h < MemoryTotals::kHeapBuckets; ++h) {
        tracked += snap.totals.bytes[h][(size_t)AllocationKind::Heap];
        tracked += snap.totals.bytes[h][(size_t)AllocationKind::Committed];
    }
    for (size_t t = 0; t < EtwSnapshot::kTypes; ++t) tracked += e.bytes[t];
    const uint64_t resident = ctr(CK::LocalResident) + ctr(CK::NonLocalResident);
    const uint64_t untracked = resident > tracked ? resident - tracked : 0;
    wchar_t untr[16]; FormatBytes(untracked, untr, 16);
    const uint64_t lost = e.session.events_lost + e.session.buffers_lost;
    PutF(0, row++, lost ? kAttrWarn : kAttrNormal,
        L"     untracked (resident - tracked) %ls   ETW objs bound %llu  unbound %llu  pending %llu   events lost %llu",
        (seen(CK::LocalResident) || seen(CK::NonLocalResident)) ? untr : L"      -  ",
        (unsigned long long)e.bound, (unsigned long long)e.unbound,
        (unsigned long long)e.pending, (unsigned long long)lost);
    return row;
}

void Renderer::Render(const Model::Snapshot& snap) {
    // Re-check console dimensions in case the user resized.
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(stdout_, &info) &&
        (info.dwSize.X != cols_ || info.dwSize.Y != rows_)) {
        Resize(info.dwSize.X, info.dwSize.Y);
    }
    Clear();

    // Header
    wchar_t uptime[32];
    FormatNs(snap.latest_ns ? (snap.latest_ns - snap.dll_start_ns) : 0, uptime, 32);
    PutF(0, 0, kAttrHeader,
        L"DX12 Track   PID %u   %ls   uptime %ls   live=%zu   bytes=%llu",
        snap.pid,
        snap.exe_path.empty() ? L"(waiting)" : snap.exe_path.c_str(),
        uptime, snap.live_count, (unsigned long long)snap.live_bytes);

    if (snap.child_exited) {
        PutF(0, 1, kAttrWarn, L"Child exited with code %u",
            snap.child_exit_code);
    }

    SHORT row = 3;
    if (snap.etw.enabled) {
        row = RenderEtwStatus(2, snap);
        row += 1;
    }
    const bool etw_on = EtwOn(snap);

    // Memory totals table. With --etw, three more columns split each row's
    // total by where ETW says the memory is.
    PutF(0, row++, kAttrAccent, etw_on
        ? L"Memory by heap / allocation kind  (VRAM / Sys / Unknown: location per ETW)"
        : L"Memory by heap / allocation kind");
    if (etw_on)
        PutF(0, row++, kAttrHeader,
            L"  %-12s %-12s %-12s %-12s %-12s %-12s %-12s %-12s %-12s",
            L"Heap", L"Heap-obj", L"Committed", L"Placed", L"Reserved", L"Total",
            L"VRAM", L"Sys", L"Unknown");
    else
        PutF(0, row++, kAttrHeader,
            L"  %-12s %-12s %-12s %-12s %-12s %-12s",
            L"Heap", L"Heap-obj", L"Committed", L"Placed", L"Reserved", L"Total");

    const wchar_t* kHeapNames[] = { L"(unknown)", L"Default", L"Upload",
                                    L"Readback",  L"Custom",  L"GpuUpload" };
    uint64_t col_totals[5] = {};
    uint64_t loc_totals[EtwSnapshot::kLocs] = {};
    uint64_t grand_total = 0;
    for (size_t h = 1; h < MemoryTotals::kHeapBuckets; ++h) {
        const auto& row_bytes = snap.totals.bytes[h];
        uint64_t row_total =
            row_bytes[(size_t)AllocationKind::Heap] +
            row_bytes[(size_t)AllocationKind::Committed] +
            row_bytes[(size_t)AllocationKind::Placed] +
            row_bytes[(size_t)AllocationKind::Reserved];
        if (row_total == 0) continue;
        wchar_t b1[16], b2[16], b3[16], b4[16], b5[16];
        FormatBytes(row_bytes[(size_t)AllocationKind::Heap],      b1, 16);
        FormatBytes(row_bytes[(size_t)AllocationKind::Committed], b2, 16);
        FormatBytes(row_bytes[(size_t)AllocationKind::Placed],    b3, 16);
        FormatBytes(row_bytes[(size_t)AllocationKind::Reserved],  b4, 16);
        FormatBytes(row_total,                                    b5, 16);
        if (etw_on) {
            const uint64_t* lb = snap.etw.loc_bytes[h];
            wchar_t l1[16], l2[16], l3[16];
            FormatBytes(lb[(size_t)MemLoc::Vram],    l1, 16);
            FormatBytes(lb[(size_t)MemLoc::Sys],     l2, 16);
            FormatBytes(lb[(size_t)MemLoc::Unknown], l3, 16);
            for (size_t l = 0; l < EtwSnapshot::kLocs; ++l) loc_totals[l] += lb[l];
            PutF(0, row++, kAttrNormal,
                L"  %-12s %-12s %-12s %-12s %-12s %-12s %-12s %-12s %-12s",
                kHeapNames[h], b1, b2, b3, b4, b5, l1, l2, l3);
        } else {
            PutF(0, row++, kAttrNormal,
                L"  %-12s %-12s %-12s %-12s %-12s %-12s",
                kHeapNames[h], b1, b2, b3, b4, b5);
        }
        col_totals[(size_t)AllocationKind::Heap]      += row_bytes[(size_t)AllocationKind::Heap];
        col_totals[(size_t)AllocationKind::Committed] += row_bytes[(size_t)AllocationKind::Committed];
        col_totals[(size_t)AllocationKind::Placed]    += row_bytes[(size_t)AllocationKind::Placed];
        col_totals[(size_t)AllocationKind::Reserved]  += row_bytes[(size_t)AllocationKind::Reserved];
        grand_total += row_total;
    }
    {
        wchar_t b1[16], b2[16], b3[16], b4[16], b5[16];
        FormatBytes(col_totals[(size_t)AllocationKind::Heap],      b1, 16);
        FormatBytes(col_totals[(size_t)AllocationKind::Committed], b2, 16);
        FormatBytes(col_totals[(size_t)AllocationKind::Placed],    b3, 16);
        FormatBytes(col_totals[(size_t)AllocationKind::Reserved],  b4, 16);
        FormatBytes(grand_total,                                   b5, 16);
        if (etw_on) {
            wchar_t l1[16], l2[16], l3[16];
            FormatBytes(loc_totals[(size_t)MemLoc::Vram],    l1, 16);
            FormatBytes(loc_totals[(size_t)MemLoc::Sys],     l2, 16);
            FormatBytes(loc_totals[(size_t)MemLoc::Unknown], l3, 16);
            PutF(0, row++, kAttrAccent,
                L"  %-12s %-12s %-12s %-12s %-12s %-12s %-12s %-12s %-12s",
                L"TOTAL", b1, b2, b3, b4, b5, l1, l2, l3);
        } else {
            PutF(0, row++, kAttrAccent,
                L"  %-12s %-12s %-12s %-12s %-12s %-12s",
                L"TOTAL", b1, b2, b3, b4, b5);
        }
    }

    // Per-type live counts, and (with --etw) driver memory next to it.
    row += 1;
    const SHORT section_top = row;
    PutF(0, row++, kAttrAccent, L"Live objects by type");
    PutF(0, row++, kAttrHeader, L"  %-20s %s", L"Type", L"Live");
    for (size_t t = 1; t < (size_t)ObjectType::Count; ++t) {
        if (snap.per_type_counts[t] == 0) continue;
        const char* n = ObjectTypeName((ObjectType)t);
        wchar_t nw[64]; size_t i = 0;
        while (n[i] && i < 63) { nw[i] = (wchar_t)n[i]; ++i; } nw[i] = 0;
        PutF(0, row++, kAttrNormal, L"  %-20s %zu", nw, snap.per_type_counts[t]);
    }
    if (etw_on) {
        constexpr SHORT x = 40;
        SHORT r = section_top;
        PutF(x, r++, kAttrAccent, L"Driver memory (ETW)");
        PutF(x, r++, kAttrHeader, L"  %-18s %7s %7s  %s", L"Type", L"Live", L"Sized", L"Bytes");
        static const struct { etw::ObjType t; const wchar_t* name; } kRows[] = {
            { etw::ObjType::PipelineState,    L"PipelineState" },
            { etw::ObjType::StateObject,      L"StateObject" },
            { etw::ObjType::DescriptorHeap,   L"DescriptorHeap" },
            { etw::ObjType::CommandAllocator, L"CommandAllocator" },
        };
        for (const auto& k : kRows) {
            const size_t t = static_cast<size_t>(k.t);
            wchar_t b[16];
            const bool na = (k.t == etw::ObjType::CommandAllocator && snap.etw.sized[t] == 0);
            if (na) swprintf(b, 16, L"     n/a");
            else FormatBytes(snap.etw.bytes[t], b, 16);
            PutF(x, r++, kAttrNormal, L"  %-18s %7llu %7llu  %ls", k.name,
                (unsigned long long)snap.etw.live[t],
                (unsigned long long)snap.etw.sized[t], b);
        }
        if (r > row) row = r;
    }

    // Recent activity
    row += 1;
    PutF(0, row++, kAttrAccent, L"Recent activity (newest at the bottom)");
    PutF(0, row++, kAttrHeader,
        L"  %-8s %-8s %-16s %-9s %-22s %s",
        L"+/-", L"id", L"type", L"size", L"heap", L"name");
    size_t shown = 0;
    const size_t available = (rows_ > row) ? (rows_ - row) : 0;
    const size_t start = (snap.recent.size() > available)
                         ? snap.recent.size() - available : 0;
    for (size_t i = start; i < snap.recent.size() && shown < available; ++i, ++shown) {
        const auto& a = snap.recent[i];
        wchar_t sz[16]; FormatBytes(a.size_bytes, sz, 16);
        const char* type_str = ObjectTypeName(a.type);
        wchar_t typew[24]; size_t j = 0;
        while (type_str[j] && j < 23) { typew[j] = (wchar_t)type_str[j]; ++j; }
        typew[j] = 0;
        PutF(0, row++, a.created ? kAttrNormal : kAttrWarn,
            L"  %-8s %-8llu %-16s %-9s %-22s %ls",
            a.created ? L"  +" : L"  -",
            (unsigned long long)a.id, typew, sz,
            a.heap_label.empty() ? L"-" : a.heap_label.c_str(),
            a.name.c_str());
    }

    // Push the buffer.
    SMALL_RECT region{0, 0, (SHORT)(cols_ - 1), (SHORT)(rows_ - 1)};
    COORD size{cols_, rows_};
    COORD origin{0, 0};
    WriteConsoleOutputW(stdout_, buffer_.data(), size, origin, &region);

    if (!dump_path_.empty()) {
        const ULONGLONG now = GetTickCount64();
        if (now - last_dump_ms_ >= 1000) {
            last_dump_ms_ = now;
            DumpNow();
        }
    }
}

void Renderer::DumpNow() {
    if (dump_path_.empty()) return;
    std::wstring text;
    text.reserve((size_t)rows_ * (cols_ + 2));
    for (SHORT y = 0; y < rows_; ++y) {
        const CHAR_INFO* line = &buffer_[(size_t)y * cols_];
        SHORT len = cols_;
        while (len > 0 && line[len - 1].Char.UnicodeChar == L' ') --len;
        for (SHORT x = 0; x < len; ++x) text.push_back(line[x].Char.UnicodeChar);
        text.append(L"\r\n");
    }
    while (text.size() >= 4 && text.compare(text.size() - 4, 4, L"\r\n\r\n") == 0)
        text.resize(text.size() - 2);
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string utf8((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                            utf8.data(), n, nullptr, nullptr);
    if (FILE* f = _wfopen(dump_path_.c_str(), L"wb")) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
}

} // namespace dx12track
