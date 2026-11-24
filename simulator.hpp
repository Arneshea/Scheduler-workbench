#pragma once
#include "resources.hpp"
#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>
#include <utility>

using Tick = std::int64_t;

enum class Cpu { Fcfs, Sjf, Priority, RoundRobin };
enum class State { New, Ready, Running, Waiting, Blocked, Suspended, Terminated };
enum class Pressure { Normal, Moderate, High, Critical };

struct Config {
    Cpu cpu = Cpu::Fcfs;
    Fit fit = Fit::First;
    Io io = Io::Fcfs;
    bool adaptive = false;
    bool lru = false;
    bool trace = false;
    int quantum = 4;
    int memory = 128;
    int frames = 8;
    int starvation = 20;
};

struct Job {
    Tick arrival;
    int priority;
    int memory;
    Tick deadline; // Absolute time, or zero for no deadline.
    std::vector<int> bursts; // CPU, disk track, CPU, ...
    std::vector<int> pages;
};

struct Process {
    Job job;
    int id;
    State state = State::New;
    std::size_t burst = 0;
    int remaining;
    Tick cpu = 0;
    Tick first = -1;
    Tick completed = -1;
    Tick waiting = 0;
    Tick ioWaiting = 0;
    Tick since = 0;
    Tick resumeAfter = 0;
    Tick order = 0;
    bool starved = false;

    Process(Job input, int pid)
        : job(std::move(input)), id(pid), remaining(job.bursts.front()) {}
};

double score(const Process& p, Tick now, double load, Pressure pressure);
Pressure pressureLevel(double utilization);

class Workload {
    mutable std::mutex mutex_;
    std::vector<Job> jobs_;

public:
    void add(Job job);
    void clear();
    std::vector<Job> snapshot() const;
};

class Simulator {
    Config cfg_;
    std::vector<Process> jobs_;
    std::vector<std::size_t> live_;
    std::size_t next_ = 0;
    Memory memory_;
    Paging pages_;
    Disk disk_;
    std::optional<std::size_t> cpu_;
    std::optional<std::size_t> previousCpu_;
    Tick now_ = 0;
    Tick sequence_ = 0;
    int slice_ = 0;
    Tick cpuTicks_ = 0, ioTicks_ = 0, switches_ = 0, starvation_ = 0;
    Tick denied_ = 0, suspensions_ = 0, deadlock_ = 0;
    double load_ = 0, memorySum_ = 0, frameSum_ = 0;
    double peakMemory_ = 0, peakFragmentation_ = 0;
    Pressure peakPressure_ = Pressure::Normal;
    std::ostream& out_;

    bool safe(Resources available, std::optional<std::size_t> cpu,
              std::optional<std::size_t> io) const;
    void ready(Process& p);
    void admit();
    void dispatchCpu();
    void dispatchIo();
    void tick();
    void report() const;

public:
    Simulator(Config config, const std::vector<Job>& jobs, std::ostream& out);
    void run();
};
