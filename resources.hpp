#pragma once
#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <vector>

using Resources = std::array<int, 3>; // CPU slots, memory units, I/O slots

struct Claim {
    Resources held;
    Resources needed;
};

// With maximum remaining claims this is Banker's safety test. With current
// outstanding requests it identifies allocations involved in a deadlock.
inline int unfinished(Resources available, const std::vector<Claim>& claims) {
    std::vector<bool> finished(claims.size());
    int remaining = static_cast<int>(claims.size());
    bool progress;
    do {
        progress = false;
        for (std::size_t i = 0; i < claims.size(); ++i) {
            if (finished[i]) continue;
            const auto& claim = claims[i];
            if (claim.needed[0] > available[0] || claim.needed[1] > available[1] ||
                claim.needed[2] > available[2]) continue;
            for (int r = 0; r < 3; ++r) available[r] += claim.held[r];
            finished[i] = true;
            --remaining;
            progress = true;
        }
    } while (progress && remaining);
    return remaining;
}

enum class Fit { First, Best, Worst };

class Memory {
    struct Block { int size; int owner; };
    std::vector<Block> blocks_;
    int capacity_;
    int used_ = 0;
    Fit fit_;

public:
    Memory(int capacity, Fit fit) : blocks_{{capacity, 0}}, capacity_(capacity), fit_(fit) {}
    int used() const { return used_; }
    double utilization() const { return static_cast<double>(used_) / capacity_; }
    double fragmentation() const {
        int largest = 0;
        for (const auto& block : blocks_) {
            if (!block.owner) largest = std::max(largest, block.size);
        }
        const int free = capacity_ - used_;
        return free ? 1.0 - static_cast<double>(largest) / free : 0.0;
    }
    bool allocate(int id, int size) {
        auto selected = blocks_.end();
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            if (it->owner || it->size < size) continue;
            if (selected == blocks_.end() ||
                (fit_ == Fit::Best && it->size < selected->size) ||
                (fit_ == Fit::Worst && it->size > selected->size)) selected = it;
            if (fit_ == Fit::First) break;
        }
        if (selected == blocks_.end()) return false;
        const int leftover = selected->size - size;
        *selected = {size, id};
        if (leftover) blocks_.insert(selected + 1, {leftover, 0});
        used_ += size;
        return true;
    }
    void release(int id) {
        auto it = std::find_if(blocks_.begin(), blocks_.end(),
                               [id](const Block& block) { return block.owner == id; });
        if (it == blocks_.end()) return;
        used_ -= it->size;
        it->owner = 0;
        if (it + 1 != blocks_.end() && !(it + 1)->owner) {
            it->size += (it + 1)->size;
            it = blocks_.erase(it + 1) - 1;
        }
        if (it != blocks_.begin() && !(it - 1)->owner) {
            (it - 1)->size += it->size;
            blocks_.erase(it);
        }
    }
};

class Paging {
    struct Frame { int pid; int page; };
    std::vector<Frame> frames_;
    std::size_t capacity_;
    bool lru_;

public:
    long long hits = 0;
    long long faults = 0;
    Paging(int capacity, bool lru) : capacity_(capacity), lru_(lru) {}
    double utilization() const { return static_cast<double>(frames_.size()) / capacity_; }
    void reference(int pid, int page) {
        const auto it = std::find_if(frames_.begin(), frames_.end(), [=](const Frame& f) {
            return f.pid == pid && f.page == page;
        });
        if (it != frames_.end()) {
            ++hits;
            if (lru_) std::rotate(it, it + 1, frames_.end());
            return;
        }
        ++faults;
        if (frames_.size() == capacity_) frames_.erase(frames_.begin());
        frames_.push_back({pid, page});
    }
    void release(int pid) {
        frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                    [=](const Frame& f) { return f.pid == pid; }), frames_.end());
    }
};

enum class Io { Fcfs, Sstf, Scan };

class Disk {
    Io policy_;
    int direction_ = 1;
    int head_ = 50;
    int left_ = 0;

public:
    std::optional<std::size_t> active;
    long long movement = 0;
    explicit Disk(Io policy) : policy_(policy) {}
    bool eligible(int track) const {
        return policy_ != Io::Scan || (track - head_) * direction_ >= 0;
    }
    int distance(int track) const {
        return policy_ == Io::Fcfs ? 0 : std::abs(track - head_);
    }
    void reverse() {
        const int edge = direction_ > 0 ? 199 : 0;
        movement += std::abs(edge - head_);
        left_ += std::abs(edge - head_);
        head_ = edge;
        direction_ *= -1;
    }
    void start(std::size_t id, int track) {
        const int seek = std::abs(track - head_);
        movement += seek;
        head_ = track;
        left_ = 1 + (seek + left_) / 10;
        active = id;
    }
    bool tick() {
        return active && --left_ == 0;
    }
};
