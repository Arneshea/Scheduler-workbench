#include "simulator.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

void require(bool passed, const char* description) {
    if (!passed) throw std::runtime_error(description);
}

std::string simulate(Config config, const std::vector<Job>& jobs) {
    std::ostringstream output;
    config.trace = true;
    Simulator(config, jobs, output).run();
    return output.str();
}

bool contains(const std::string& text, const char* expected) {
    return text.find(expected) != std::string::npos;
}

int main() {
    try {
        Config config;
        const std::vector<Job> cpu = {
            {0, 1, 10, 0, {6}, {}}, {0, 0, 10, 0, {3}, {}},
            {1, 4, 10, 0, {2}, {}}, {2, 2, 10, 0, {1}, {}}
        };
        auto result = simulate(config, cpu);
        require(contains(result, "[6] CPU P2\n[9] CPU P3\n[11] CPU P4") &&
                contains(result, "Average waiting: 5.75"), "FCFS order and waits");
        config.cpu = Cpu::RoundRobin;
        result = simulate(config, cpu);
        require(contains(result, "[4] CPU P2\n[7] CPU P3\n[9] CPU P4\n[10] CPU P1") &&
                contains(result, "Average turnaround: 8.75"), "Round Robin quantum and turnaround");
        config.cpu = Cpu::Sjf;
        require(contains(simulate(config, cpu), "[0] CPU P2"), "SJF shortest ready burst");
        config.cpu = Cpu::Priority;
        require(contains(simulate(config, cpu), "[6] CPU P3"), "Priority ordering");

        Process aging({0, 1, 10, 0, {3}, {}}, 1);
        Process urgent({0, 1, 10, 30, {3}, {}}, 2);
        require(score(aging, 30, .8, Pressure::Normal) >
                score(aging, 0, .8, Pressure::Normal), "Adaptive aging");
        require(score(urgent, 25, 0, Pressure::Normal) - score(aging, 25, 0, Pressure::Normal) >
                score(urgent, 0, 0, Pressure::Normal) - score(aging, 0, 0, Pressure::Normal),
                "Adaptive deadline urgency");
        require(pressureLevel(.96) == Pressure::Critical, "Critical pressure");

        for (auto fit : {Fit::First, Fit::Best, Fit::Worst}) {
            Memory memory(25, fit);
            require(memory.allocate(1, 10) && memory.allocate(2, 5) &&
                    memory.allocate(3, 6) && memory.allocate(4, 4), "Memory allocation");
            memory.release(1);
            memory.release(3);
            require(memory.allocate(5, 5), "Memory reallocation");
            require(memory.allocate(6, fit == Fit::Best ? 10 : 6), "Fit selects expected hole");
            for (int id : {2, 4, 5, 6}) memory.release(id);
            require(memory.fragmentation() == 0 && memory.allocate(7, 25), "Memory coalescing");
        }
        require(contains(simulate(Config{}, {{0, 1, 123, 0, {2}, {}}}),
                         "Peak memory pressure: CRITICAL"), "Measured memory pressure");
        Config pressure;
        pressure.adaptive = true;
        pressure.memory = 10;
        result = simulate(pressure, {{0, 0, 9, 0, {5}, {}}, {0, 1, 1, 0, {1}, {}},
                                    {1, 100, 6, 0, {1}, {}}});
        require(contains(result, "[1] suspend P1") && contains(result, "Completed: 3/3"),
                "Adaptive suspension and resumption");

        Paging lru(3, true), fifo(3, false);
        for (int page : {1, 2, 3, 1, 4, 1, 2}) {
            lru.reference(1, page);
            fifo.reference(1, page);
        }
        require(lru.faults == 5 && lru.hits == 2 && fifo.faults == 6, "Page replacement");

        config = {};
        config.io = Io::Sstf;
        const std::vector<Job> io = {
            {0, 0, 1, 0, {1, 150, 1}, {}}, {0, 0, 1, 0, {1, 20, 1}, {}},
            {0, 0, 1, 0, {1, 160, 1}, {}}
        };
        result = simulate(config, io);
        require(contains(result, "I/O P3 track 160") &&
                result.find("I/O P3") < result.find("I/O P2"), "SSTF service order");
        config.io = Io::Scan;
        require(contains(simulate(config, io), "Disk head movement: 328 tracks"), "SCAN endpoint reversal");
        config.io = Io::Fcfs;
        result = simulate(config, io);
        require(result.find("I/O P2") < result.find("I/O P3"), "Disk FCFS queue order");

        config.memory = 10;
        result = simulate(config, {{0, 0, 7, 0, {3}, {}}, {0, 0, 7, 0, {1}, {}}});
        require(contains(result, "[3] admit P2") &&
                contains(result, "Deferred allocation attempts: 3"), "Insufficient resources defer admission");
        const std::vector<Claim> conflict = {
            {{1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}}
        };
        require(unfinished({0, 10, 0}, conflict) == 2 &&
                unfinished({1, 10, 0}, conflict) == 0, "Circular wait detection");

        Workload workload;
        const Job job{0, 0, 1, 0, {1}, {}};
        auto produce = [&] { for (int i = 0; i < 100; ++i) workload.add(job); };
        std::thread a(produce), b(produce);
        a.join();
        b.join();
        require(workload.snapshot().size() == 200, "Concurrent workload producers");
        config.adaptive = true;
        result = simulate(config, io);
        require(contains(result, "Completed: 3/3") && contains(result, "Deadlocks: 0"),
                "Adaptive CPU, memory, paging and disk integration");

        std::cout << "All scheduler tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}


