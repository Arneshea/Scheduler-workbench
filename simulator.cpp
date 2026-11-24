#include "simulator.hpp"
#include <iomanip>
#include <ostream>
#include <tuple>

Pressure pressureLevel(double used) {
    if (used >= .95) return Pressure::Critical;
    if (used >= .80) return Pressure::High;
    if (used >= .60) return Pressure::Moderate;
    return Pressure::Normal;
}

double score(const Process& p, Tick now, double load, Pressure pressure) {
    double urgency = 0;
    if (p.job.deadline) {
        Tick work = p.remaining;
        for (std::size_t i = p.burst + 2; i < p.job.bursts.size(); i += 2) {
            work += p.job.bursts[i];
        }
        const Tick slack = p.job.deadline - now - work;
        urgency = std::clamp(20.0 - static_cast<double>(slack), 0.0, 40.0);
    }
    const double age = static_cast<double>(now - p.since);
    return p.job.priority + age * (1 + load) + urgency -
           load * std::min<Tick>(p.cpu, 100) / 4.0 -
           static_cast<int>(pressure) * p.job.memory / 64.0;
}

void Workload::add(Job job) {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
}
void Workload::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
}
std::vector<Job> Workload::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_;
}

Simulator::Simulator(Config config, const std::vector<Job>& jobs, std::ostream& out)
    : cfg_(config), memory_(config.memory, config.fit),
      pages_(config.frames, config.lru), disk_(config.io), out_(out) {
    int id = 0;
    for (const auto& job : jobs) jobs_.emplace_back(job, ++id);
    std::stable_sort(jobs_.begin(), jobs_.end(), [](const Process& a, const Process& b) {
        return a.job.arrival < b.job.arrival;
    });
}

bool Simulator::safe(Resources available, std::optional<std::size_t> cpu,
                     std::optional<std::size_t> io) const {
    if (available[0] < 0 || available[1] < 0 || available[2] < 0) return false;

    // Every admitted process already holds its full memory claim. A CPU or
    // disk holder releases its slot before requesting the other, so waiting
    // processes need at most one of each slot once the holders finish.
    const std::vector<Claim> claims = {
        {{cpu ? 1 : 0, 0, io ? 1 : 0}, {0, 0, 0}},
        {{0, cfg_.memory - available[1], 0}, {1, 0, 1}}
    };
    return unfinished(available, claims) == 0;
}

void Simulator::ready(Process& p) {
    p.state = State::Ready;
    p.since = now_;
    p.order = sequence_++;
}

void Simulator::admit() {
    while (next_ < jobs_.size() && jobs_[next_].job.arrival == now_) {
        auto& p = jobs_[next_];
        p.state = State::Blocked;
        p.since = now_;
        live_.push_back(next_++);
    }
    std::vector<std::size_t> pending;
    for (auto i : live_) {
        const auto& p = jobs_[i];
        if ((p.state == State::Blocked || p.state == State::Suspended) &&
            p.resumeAfter <= now_) pending.push_back(i);
    }
    const auto pressure = pressureLevel(memory_.utilization());
    auto effective = [&](std::size_t i) { return score(jobs_[i], now_, load_, pressure); };
    if (cfg_.adaptive) {
        std::stable_sort(pending.begin(), pending.end(), [&](auto a, auto b) {
            return effective(a) > effective(b);
        });
    }
    bool displaced = false;
    for (auto i : pending) {
        auto& p = jobs_[i];
        auto allocate = [&] {
            return safe({cpu_ ? 0 : 1, cfg_.memory - memory_.used() - p.job.memory,
                         disk_.active ? 0 : 1}, cpu_, disk_.active) &&
                   memory_.allocate(p.id, p.job.memory);
        };
        bool granted = allocate();
        if (!granted && cfg_.adaptive && !displaced && p.state == State::Blocked &&
            pressureLevel(memory_.utilization()) >= Pressure::High) {
            std::optional<std::size_t> victim;
            for (auto j : live_) {
                if (jobs_[j].state != State::Ready || effective(j) >= effective(i)) continue;
                if (!victim || effective(j) < effective(*victim)) victim = j;
            }
            if (victim) {
                auto& other = jobs_[*victim];
                Memory proposed = memory_;
                proposed.release(other.id);
                if (!proposed.allocate(p.id, p.job.memory)) {
                    ++denied_;
                    continue;
                }
                other.state = State::Suspended;
                other.resumeAfter = now_ + cfg_.quantum;
                memory_.release(other.id);
                pages_.release(other.id);
                ++suspensions_;
                displaced = true;
                if (cfg_.trace) out_ << '[' << now_ << "] suspend P" << other.id << '\n';
                granted = allocate();
            }
        }
        if (granted) {
            ready(p);
            if (cfg_.trace) out_ << '[' << now_ << "] admit P" << p.id << '\n';
        } else {
            ++denied_;
        }
    }
}

void Simulator::dispatchCpu() {
    if (cpu_ && !cfg_.adaptive && cfg_.cpu != Cpu::RoundRobin) return;
    if (cpu_ && slice_ < cfg_.quantum) return;
    if (cpu_) {
        ready(jobs_[*cpu_]);
        cpu_.reset();
    }
    const auto pressure = pressureLevel(memory_.utilization());
    auto rank = [&](const Process& p) -> double {
        if (cfg_.adaptive) return -score(p, now_, load_, pressure);
        if (cfg_.cpu == Cpu::Sjf) return p.remaining;
        if (cfg_.cpu == Cpu::Priority) return -p.job.priority;
        return 0;
    };
    std::optional<std::size_t> selected;
    for (auto i : live_) {
        const auto& p = jobs_[i];
        if (p.state != State::Ready) continue;
        if (!selected || std::make_pair(rank(p), p.order) <
                         std::make_pair(rank(jobs_[*selected]), jobs_[*selected].order)) {
            selected = i;
        }
    }
    if (!selected) return;
    if (!safe({0, cfg_.memory - memory_.used(), disk_.active ? 0 : 1},
              selected, disk_.active)) {
        ++denied_;
        return;
    }
    auto& p = jobs_[*selected];
    cpu_ = selected;
    slice_ = 0;
    p.state = State::Running;
    if (p.first < 0) p.first = now_;
    if (previousCpu_ != cpu_) {
        ++switches_;
        if (cfg_.trace) out_ << '[' << now_ << "] CPU P" << p.id << '\n';
    }
    previousCpu_ = cpu_;
}

void Simulator::dispatchIo() {
    if (disk_.active) return;
    bool pending = false;
    const auto pressure = pressureLevel(memory_.utilization());
    auto select = [&]() {
        std::optional<std::size_t> best;
        auto rank = [&](const Process& p) {
            const double distance = disk_.distance(p.job.bursts[p.burst + 1]);
            const double boost = cfg_.adaptive ? score(p, now_, load_, pressure) : 0;
            if (cfg_.io == Io::Scan) return std::make_tuple(distance, -boost, p.order);
            return std::make_tuple(distance - boost, 0.0, p.order);
        };
        for (auto i : live_) {
            const auto& p = jobs_[i];
            if (p.state != State::Waiting) continue;
            pending = true;
            if (!disk_.eligible(p.job.bursts[p.burst + 1])) continue;
            if (!best || rank(p) < rank(jobs_[*best])) best = i;
        }
        return best;
    };
    auto selected = select();
    if (!selected && pending) {
        disk_.reverse();
        selected = select();
    }
    if (!selected) return;
    if (!safe({cpu_ ? 0 : 1, cfg_.memory - memory_.used(), 0}, cpu_, selected)) {
        ++denied_;
        return;
    }
    const auto& p = jobs_[*selected];
    const int track = p.job.bursts[p.burst + 1];
    disk_.start(*selected, track);
    if (cfg_.trace) out_ << '[' << now_ << "] I/O P" << p.id << " track " << track << '\n';
}

void Simulator::tick() {
    admit();
    dispatchCpu();
    dispatchIo();

    for (auto i : live_) {
        auto& p = jobs_[i];
        const bool ioQueued = p.state == State::Waiting && i != disk_.active;
        const bool waiting = ioQueued || p.state == State::Ready ||
                             p.state == State::Blocked || p.state == State::Suspended;
        if (p.state == State::Ready || p.state == State::Blocked || p.state == State::Suspended) {
            ++p.waiting;
        }
        if (ioQueued) ++p.ioWaiting;
        if (waiting && !p.starved && now_ - p.since + 1 >= cfg_.starvation) {
            p.starved = true;
            ++starvation_;
        }
    }
    memorySum_ += memory_.utilization();
    peakMemory_ = std::max(peakMemory_, memory_.utilization());
    peakFragmentation_ = std::max(peakFragmentation_, memory_.fragmentation());
    peakPressure_ = std::max(peakPressure_, pressureLevel(memory_.utilization()));
    load_ = .9 * load_ + (cpu_ ? .1 : 0);
    ++now_;

    if (disk_.active) {
        ++ioTicks_;
        if (disk_.tick()) {
            auto& p = jobs_[*disk_.active];
            disk_.active.reset();
            p.burst += 2;
            p.remaining = p.job.bursts[p.burst];
            ready(p);
        }
    }
    if (cpu_) {
        auto& p = jobs_[*cpu_];
        const int page = p.job.pages.empty() ? static_cast<int>((p.cpu / 3) % 8) :
                         p.job.pages[static_cast<std::size_t>(p.cpu) % p.job.pages.size()];
        pages_.reference(p.id, page);
        ++p.cpu;
        ++cpuTicks_;
        ++slice_;
        const bool finishedBurst = --p.remaining == 0;
        frameSum_ += pages_.utilization();
        if (finishedBurst) {
            cpu_.reset();
            if (p.burst + 1 < p.job.bursts.size()) {
                p.state = State::Waiting;
                p.since = now_;
                p.order = sequence_++;
            } else {
                p.state = State::Terminated;
                p.completed = now_;
                memory_.release(p.id);
                pages_.release(p.id);
            }
        }
    } else {
        previousCpu_.reset();
        frameSum_ += pages_.utilization();
    }
}

void Simulator::run() {
    while (next_ < jobs_.size() || !live_.empty()) {
        if (live_.empty() && now_ < jobs_[next_].job.arrival) {
            const auto gap = jobs_[next_].job.arrival - now_;
            load_ = 0;
            previousCpu_.reset();
            if (gap > 0) now_ += gap;
        }
        tick();
        live_.erase(std::remove_if(live_.begin(), live_.end(), [&](auto i) {
            return jobs_[i].state == State::Terminated;
        }), live_.end());
        if (!cpu_ && !disk_.active && !live_.empty()) {
            bool canProgress = false;
            std::vector<Claim> requests;
            for (auto i : live_) {
                const auto& p = jobs_[i];
                if (p.state == State::Ready || p.state == State::Waiting ||
                    (p.state == State::Suspended && p.resumeAfter > now_)) canProgress = true;
                if (p.state == State::Blocked || p.state == State::Suspended) {
                    requests.push_back({{0, 0, 0}, {0, p.job.memory, 0}});
                }
            }
            if (!canProgress && unfinished({1, cfg_.memory - memory_.used(), 1}, requests)) {
                ++deadlock_;
                break;
            }
        }
    }
    report();
}

void Simulator::report() const {
    Tick waiting = 0, turnaround = 0, response = 0, ioWaiting = 0;
    int complete = 0, misses = 0;
    out_ << "PID   State          Arrival  Finish  Wait  Turn  Response  I/O wait\n";
    for (const auto& p : jobs_) {
        if (p.job.deadline &&
            (p.completed >= 0 ? p.completed > p.job.deadline : now_ >= p.job.deadline)) ++misses;
        if (p.completed >= 0) {
            ++complete;
            waiting += p.waiting;
            turnaround += p.completed - p.job.arrival;
            response += p.first - p.job.arrival;
            ioWaiting += p.ioWaiting;
        }
        static const char* states[] = {
            "NEW", "READY", "RUNNING", "WAITING", "BLOCKED", "SUSPENDED", "TERMINATED"
        };
        out_ << std::left << std::setw(6) << p.id << std::setw(15)
             << states[static_cast<int>(p.state)] << std::right << std::setw(7) << p.job.arrival
             << ' ' << std::setw(8) << p.completed << ' ' << std::setw(6) << p.waiting
             << ' ' << std::setw(6) << (p.completed < 0 ? -1 : p.completed - p.job.arrival)
             << ' ' << std::setw(10) << (p.first < 0 ? -1 : p.first - p.job.arrival)
             << ' ' << std::setw(10) << p.ioWaiting << '\n';
    }
    const double time = now_ ? static_cast<double>(now_) : 1.0;
    const double count = complete ? complete : 1.0;
    static const char* pressure[] = {"NORMAL", "MODERATE", "HIGH", "CRITICAL"};
    out_ << std::fixed << std::setprecision(2)
         << "\nElapsed ticks: " << now_ << " | Completed: " << complete << '/' << jobs_.size()
         << "\nCPU utilization: " << 100 * cpuTicks_ / time << '%'
         << "\nMemory utilization: " << 100 * memorySum_ / time << "% (peak " << 100 * peakMemory_ << "%)"
         << "\nI/O utilization: " << 100 * ioTicks_ / time << '%'
         << "\nOverall utilization: " << 100 * (cpuTicks_ + ioTicks_ + memorySum_) / (3 * time) << '%'
         << "\nPeak memory pressure: " << pressure[static_cast<int>(peakPressure_)]
         << "\nPeak external fragmentation: " << 100 * peakFragmentation_ << '%'
         << "\nAverage waiting: " << waiting / count << " ticks"
         << "\nAverage turnaround: " << turnaround / count << " ticks"
         << "\nAverage response: " << response / count << " ticks"
         << "\nAverage I/O wait: " << ioWaiting / count << " ticks"
         << "\nContext switches: " << switches_
         << "\nPage faults: " << pages_.faults << " | Page hits: " << pages_.hits
         << "\nAverage frame utilization: " << 100 * frameSum_ / time << '%'
         << "\nDisk head movement: " << disk_.movement << " tracks"
         << "\nDeadline misses: " << misses << " | Starvation events: " << starvation_
         << "\nDeferred allocation attempts: " << denied_ << " | Suspensions: " << suspensions_
         << " | Deadlocks: " << deadlock_ << "\n\n";
}
