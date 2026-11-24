# Adaptive Multi-Resource OS Scheduler

A C++17 simulator for a single CPU, one disk, contiguous memory, and shared
page frames. Processes alternate between CPU bursts and disk requests while
holding their memory allocation. Disk tracks range from 0 to 199. The head
starts at track 50 and service takes one tick plus one tick per ten tracks
moved, including travel to an endpoint when SCAN reverses.

## Build and run

On Linux with GNU g++ and Make:

```sh
make
./scheduler
make test
```

On Windows with MinGW g++ and PowerShell:

```powershell
g++ -std=c++17 -pthread -O2 main.cpp simulator.cpp -o scheduler.exe
.\scheduler.exe
```

Enter `help` for the full command list. These commands run a mixed workload
twice so you can compare the baseline against adaptive scheduling:

```text
set memory 64
set frames 4
set io sstf
set trace on
generate mixed 8 42
run
set adaptive on
set paging lru
run
quit
```

Enter custom processes with `add ARRIVAL PRIORITY MEMORY DEADLINE BURSTS [PAGES]`.
For example, `add 0 5 24 40 4,75,6 0,1,2,0,3` uses 24 memory units and asks for
four CPU ticks, a disk request at track 75, and another six CPU ticks. Its
absolute deadline is tick 40. Use deadline 0 to disable it. Higher priority
numbers are favored. The page sequence loops until completion. Runs replay
the existing workload from tick zero. Use `clear` before defining a new workload.
Commands can also be read from standard input.

## How the simulation works

- FCFS, SJF, and Priority run each CPU burst without preemption. Round Robin
  and adaptive scheduling reconsider dispatch at each quantum boundary.
  Adaptive mode uses a score based on priority, queue age, recent CPU
  utilization, CPU consumption, memory pressure, and deadline slack.
- Memory pressure thresholds are 60%, 80%, and 95%. Under high or critical
  pressure, adaptive mode can suspend a lower-scoring ready process when
  doing so makes room for a blocked process. Suspended processes keep their
  CPU progress and can resume after a quantum.
- Memory allocation uses First Fit, Best Fit, or Worst Fit with adjacent
  free-block coalescing. Global FIFO or LRU paging records hits and faults.
  Page faults contribute to counters and leave CPU timing unchanged.
- Disk scheduling supports FCFS, SSTF, and SCAN. Adaptive mode factors score
  into request selection. SCAN preserves direction and track order and uses
  score to prioritize requests at the same track.
- Resource safety uses the completion algorithm behind Banker's safety
  check. Processes request their full memory claim on admission and release
  each CPU or disk slot before needing another. This acquisition model
  prevents circular waits in ordinary workloads. The tests inject a
  conflicting set of claims into the detection algorithm.
- Waiting time includes admission, ready, and suspension delays. I/O queue
  wait is measured separately. Response time ends at first CPU dispatch.
  Context switches count dispatches to a different process or from idle.
  Overall utilization averages CPU, memory, and disk utilization.
- The workload is mutex-protected for concurrent producers. Each simulation
  runs on a private snapshot and advances on one thread for repeatability.

## Browser dashboard

On Windows, double-click **Start-Dashboard.cmd** to start the dashboard and
open the browser. This uses Node.js and the built scheduler executable.

You can also start the dashboard manually:

```sh
node web/server.mjs
```

Open http://127.0.0.1:4173 in your browser. The dashboard starts with an example
comparison. Edit the process rows, choose two CPU policies, and select
**Run comparison**. Both policies receive the same workload and resource
settings. The results include charts, full metrics, and per-process timings.
**Load example** restores the sample workload and settings.

Keep the server running during the demo. Press Ctrl+C to close it.
