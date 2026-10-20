/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace netease::playback {
/* Per-decoder total, including allocation headers. The upstream callbacks
 * return NULL on exhaustion, preserving the running service and its queue. */
class AllocationBudget {
    struct alignas(std::max_align_t) Header { size_t bytes; };
public:
    static constexpr size_t Limit = 4 * 1024 * 1024;
    size_t used = 0, peak = 0;
    bool limited = false;
    static void* allocate(size_t size, void* context) {
        auto& budget = *static_cast<AllocationBudget*>(context);
        if (size > Limit - sizeof(Header) || size + sizeof(Header) > Limit - budget.used) {
            budget.limited = true;
            return nullptr;
        }
        auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
        if (!header) { budget.limited = true; return nullptr; }
        header->bytes = sizeof(Header) + size;
        budget.used += header->bytes;
        budget.peak = std::max(budget.peak, budget.used);
        return header + 1;
    }
    static void release(void* memory, void* context) {
        if (!memory) return;
        auto& budget = *static_cast<AllocationBudget*>(context);
        auto* header = static_cast<Header*>(memory) - 1;
        budget.used -= header->bytes;
        std::free(header);
    }
    static void* resize(void* memory, size_t size, void* context) {
        if (!memory) return allocate(size, context);
        if (!size) { release(memory, context); return nullptr; }
        auto& budget = *static_cast<AllocationBudget*>(context);
        auto* header = static_cast<Header*>(memory) - 1;
        const size_t previous = header->bytes;
        if (size > Limit - sizeof(Header) ||
            size + sizeof(Header) > Limit - (budget.used - previous)) {
            budget.limited = true;
            return nullptr;
        }
        auto* replacement = static_cast<Header*>(std::realloc(header, sizeof(Header) + size));
        if (!replacement) { budget.limited = true; return nullptr; }
        replacement->bytes = sizeof(Header) + size;
        budget.used = budget.used - previous + replacement->bytes;
        budget.peak = std::max(budget.peak, budget.used);
        return replacement + 1;
    }
};
}
