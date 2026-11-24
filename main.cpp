#include "simulator.hpp"
#include <charconv>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

int number(std::string_view value, int min, int max) {
    int n;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), n);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || n < min || n > max)
        throw std::runtime_error("Expected an integer in [" + std::to_string(min) + ", " +
                                 std::to_string(max) + "]: " + std::string(value));
    return n;
}

std::vector<int> list(std::string_view input, bool bursts) {
    std::vector<int> values;
    for (;;) {
        const auto comma = input.find(',');
        const bool track = bursts && values.size() % 2 == 1;
        values.push_back(number(input.substr(0, comma), bursts && !track ? 1 : 0,
                                track ? 199 : 1000000));
        if (comma == input.npos) break;
        input.remove_prefix(comma + 1);
    }
    if (bursts && values.size() % 2 == 0)
        throw std::runtime_error("Bursts must end with a CPU duration");
    return values;
}

template<class T>
T option(const std::string& value, std::initializer_list<std::pair<const char*, T>> options) {
    for (const auto& entry : options) if (value == entry.first) return entry.second;
    throw std::runtime_error("Unknown option value: " + value);
}

void configure(Config& cfg, const std::string& key, const std::string& value) {
    if (key == "cpu") {
        cfg.cpu = option<Cpu>(value, {{"fcfs", Cpu::Fcfs}, {"sjf", Cpu::Sjf},
                                    {"priority", Cpu::Priority}, {"rr", Cpu::RoundRobin}});
    } else if (key == "fit") {
        cfg.fit = option<Fit>(value, {{"first", Fit::First}, {"best", Fit::Best}, {"worst", Fit::Worst}});
    } else if (key == "io") {
        cfg.io = option<Io>(value, {{"fcfs", Io::Fcfs}, {"sstf", Io::Sstf}, {"scan", Io::Scan}});
    } else if (key == "paging") {
        cfg.lru = option<bool>(value, {{"fifo", false}, {"lru", true}});
    } else if (key == "adaptive" || key == "trace") {
        (key == "adaptive" ? cfg.adaptive : cfg.trace) =
            option<bool>(value, {{"off", false}, {"on", true}});
    } else if (key == "memory") cfg.memory = number(value, 1, 1000000);
    else if (key == "frames") cfg.frames = number(value, 1, 10000);
    else if (key == "quantum") cfg.quantum = number(value, 1, 1000000);
    else if (key == "starvation") cfg.starvation = number(value, 1, 1000000);
    else throw std::runtime_error("Unknown setting: " + key);
}

enum class Profile { Cpu, Memory, Io, Mixed, Spike, Burst };

void generate(Workload& workload, const Config& cfg, Profile profile, int count, int seed) {
    std::mt19937 random(seed);
    auto draw = [&](int low, int high) { return low + static_cast<int>(random() % (high - low + 1)); };
    for (int i = 0; i < count; ++i) {
        const int arrival = profile == Profile::Burst ? 0 :
                            profile == Profile::Spike && i >= count / 3 ? 12 : i * 2;
        const bool heavy = profile == Profile::Memory || (profile == Profile::Spike && arrival == 12);
        const int memory = heavy ? draw(std::max(1, cfg.memory / 4), std::max(1, cfg.memory * 3 / 4)) :
                                   draw(1, std::max(1, cfg.memory / 5));
        Job job{arrival, draw(0, 15), memory, arrival + draw(25, 120), {}, {}};
        if (profile == Profile::Cpu) job.bursts = {draw(15, 40)};
        else if (profile == Profile::Io)
            job.bursts = {draw(1, 4), draw(0, 199), draw(1, 4), draw(0, 199), draw(1, 4)};
        else if (draw(0, 1)) job.bursts = {draw(2, 12), draw(0, 199), draw(2, 12)};
        else job.bursts = {draw(3, 20)};
        workload.add(std::move(job));
    }
}

void help() {
    std::cout <<
        "add ARRIVAL PRIORITY MEMORY DEADLINE BURSTS [PAGES]\n"
        "    BURSTS is a comma-separated CPU duration, disk track, CPU duration, ...\n"
        "    PAGES is an optional comma-separated reference sequence. Deadline 0 disables it.\n"
        "    Example: add 0 5 24 40 4,75,6 0,1,2,0,3\n"
        "generate cpu|memory|io|mixed|spike|burst COUNT [SEED]\n"
        "set cpu fcfs|sjf|priority|rr     set quantum N\n"
        "set fit first|best|worst         set memory N\n"
        "set paging fifo|lru              set frames N\n"
        "set io fcfs|sstf|scan            set adaptive on|off\n"
        "set starvation N                set trace on|off\n"
        "run                              clear\n"
        "help                             quit\n";
}

bool execute(const std::vector<std::string>& args, Config& cfg, Workload& workload) {
    const auto& cmd = args.front();
    const auto count = args.size() - 1;
    if (cmd == "quit" && count == 0) return false;
    if (cmd == "help" && count == 0) help();
    else if (cmd == "clear" && count == 0) workload.clear();
    else if (cmd == "set" && count == 2) configure(cfg, args[1], args[2]);
    else if (cmd == "add" && (count == 5 || count == 6)) {
        workload.add({number(args[1], 0, 1000000000), number(args[2], -1000000, 1000000),
                      number(args[3], 1, 1000000), number(args[4], 0, 1000000000),
                      list(args[5], true), count == 6 ? list(args[6], false) : std::vector<int>{}});
    } else if (cmd == "generate" && (count == 2 || count == 3)) {
        const auto profile = option<Profile>(args[1], {{"cpu", Profile::Cpu}, {"memory", Profile::Memory},
            {"io", Profile::Io}, {"mixed", Profile::Mixed}, {"spike", Profile::Spike}, {"burst", Profile::Burst}});
        generate(workload, cfg, profile, number(args[2], 1, 10000),
                 count == 3 ? number(args[3], 0, 1000000000) : 42);
    } else if (cmd == "run" && count == 0) {
        const auto jobs = workload.snapshot();
        if (jobs.empty()) throw std::runtime_error("Add or generate at least one process");
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].memory > cfg.memory)
                throw std::runtime_error("P" + std::to_string(i + 1) + " requests " +
                    std::to_string(jobs[i].memory) + " memory units, exceeding capacity " +
                    std::to_string(cfg.memory));
        }
        Simulator(cfg, jobs, std::cout).run();
    } else {
        throw std::runtime_error("Unknown command or wrong argument count. Enter help for syntax");
    }
    return true;
}

int main() {
    Config config;
    Workload workload;
    bool failed = false;
    std::cout << "Adaptive Multi-Resource OS Scheduler | Enter help for commands.\n";
    for (std::string line; std::getline(std::cin, line);) {
        if (const auto comment = line.find('#'); comment != line.npos) line.erase(comment);
        std::istringstream input(line);
        std::vector<std::string> args;
        for (std::string arg; input >> arg;) args.push_back(std::move(arg));
        if (args.empty()) continue;
        try {
            if (!execute(args, config, workload)) break;
        } catch (const std::runtime_error& error) {
            std::cerr << "Error: " << error.what() << '\n';
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
