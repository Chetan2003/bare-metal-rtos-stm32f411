# Bare-Metal RTOS on STM32F411RE

A small preemptive real-time kernel written from scratch in C and ARM assembly for the
STM32F411RE (Cortex-M4, Nucleo-F411RE). The kernel itself is register-level and uses no
HAL or third-party OS; the HAL is only used for the initial clock setup that STM32CubeMX
generates.

It is built to test two ideas on real hardware:

1. **Rate-Monotonic (RM) scheduling**: measure each task's response time and compare it
   with response-time analysis.
2. **Priority Inheritance Protocol (PIP)**: reproduce priority inversion, then bound it by
   enabling inheritance with a single compile-time flag.

## What is in the kernel

| Piece | Where |
|---|---|
| Task control block, mutex, public API | `Core/Inc/kernel.h` |
| Stack-frame setup, scheduler, PendSV context switch, tick, sleep, mutex + PIP | `Core/Src/kernel.c` |
| USART2 driver (ST-LINK virtual COM port) | `Core/Src/uart.c` |
| The three experiments, load calibration, log buffer | `Core/Src/app.c` |

- **Context switch:** each task has its own stack. A suspended task is 16 words on that
  stack (8 saved by hardware on exception entry, R4-R11 saved by `PendSV_Handler`). New
  tasks get a hand-built initial frame.
- **Scheduler:** fixed priority (lower number = higher priority). Equal priorities
  round-robin. Driven by a 1 ms SysTick, plus an immediate switch when a task sleeps,
  locks or unlocks.
- **Periodic tasks:** release times are absolute (`next += period`), so there is no drift.
  Response time = finish time minus the release time stamped in the tick handler, read from
  the DWT cycle counter.
- **Mutex:** single owner, no recursion. With `PRIORITY_INHERITANCE_ENABLED` set to 1,
  a blocked high-priority task boosts the owner's priority; unlock restores it and hands
  the lock to the highest-priority waiter.

## Experiments

Choose one with `DEMO` in `Core/Inc/app.h`:

| `DEMO` | What it runs |
|---|---|
| `DEMO_CONTEXT_SWITCH` | Two equal-priority tasks with different loop lengths; compare their counters in the debugger. |
| `DEMO_RM_SCHEDULER` | T1/T2/T3 with 10/20/50 ms periods; prints `Tn,<job>,<response_us>` over UART. |
| `DEMO_PRIORITY_INV` | H/M/L tasks sharing a mutex; prints `H,<wait_us>,<M preemptions>`. Build once with `PRIORITY_INHERITANCE_ENABLED` 0 and once with 1. |

The busy-loop loads are calibrated against the DWT cycle counter at boot. The first UART
line reports the calibration (`cycles/iter x256`).

## Build and run

Requires the Arm GNU toolchain, CMake and Ninja (the STM32 VS Code extension installs
these as bundles).

```
cmake --preset Debug
cmake --build --preset Debug
```

Debug with the **STM32Cube: Debug RTOS (ST-Link)** launch configuration in VS Code.
UART output: ST-LINK virtual COM port, 115200 8N1.

## Measured results (Nucleo-F411RE, 84 MHz, `-O0`)

Raw UART captures are in [`results/`](results/). The busy-loop calibration measured 11.0
CPU cycles per iteration.

**Context switch.** Counters of the two equal-priority tasks, read over SWD while running:
646 / 215 and, 20 s later, 1040 / 346, a ratio of 3.01 against the 3.00 expected from the
600,000 : 200,000 loop lengths.

**Rate-monotonic scheduler** (3,328 jobs, no deadline misses, no dropped log lines):

| Task | Period | Jobs | Measured response (µs) | Predicted worst case (µs) |
|---|---|---|---|---|
| T1 | 10 ms | 1,957 | 1414 - 1420 | 1414 |
| T2 | 20 ms | 979 | 8863 - 8870 | 8870 |
| T3 | 50 ms | 392 | 6452 - 15330 | 15322 |

The execution times used for the prediction were derived from the same run (C1 from T1,
C2 from T2's worst case, C3 from T3's best case), so T2's match is a self-consistency check;
T3's worst case, predicted from the other three figures, is the independent test: it lands
within 8 µs of the measurement. T3 shows two response-time clusters (about 6.45 ms and 15.3 ms)
depending on where its release falls in the 100 ms hyperperiod.

**Priority inversion** (H: 100 ms period, M: 5 ms period with a 1 ms load, L: 95 ms period
holding the mutex for 25 ms of work; about 208 H releases per run):

| | Max wait of H (µs) | Mean wait when blocked (µs) |
|---|---|---|
| PIP off | 27,247 | 14,748 |
| PIP on | 21,172 | 11,205 |

Without inheritance H waits 2.2 ms longer than L's whole critical section, and the extra
6.1 ms matches six runs of M (about 1.01 ms each), a task that never touches the mutex.
With inheritance H's worst wait stays below the critical-section length. The maxima were
identical in all 11 drift cycles of each run.

## Limitations

- The kernel does not save FPU registers on a context switch, so tasks must not use
  floating point.
- Scheduling decisions happen at 1 ms tick granularity unless a task triggers one itself.
- The mutex does not handle nesting or transitive inheritance.
