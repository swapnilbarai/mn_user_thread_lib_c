# M:N User-Level Threading Library

A lightweight **M:N user-level threading library written in C and x86-64 assembly** for Linux.

The project implements user-space threads that are multiplexed across a pool of kernel threads. Multiple user threads (`N`) can execute on multiple kernel worker threads (`M`), providing an **M:N threading model**.

The project is designed as a low-level systems programming project to explore:

* User-level thread scheduling
* M:N threading
* Context switching
* x86-64 assembly
* Linux `clone()` / system calls
* Thread synchronization
* Thread `yield`
* Thread `join`
* User-level thread stacks
* Kernel worker threads
* Barriers and synchronization primitives
* Lock-free / MPMC data structures
* C/C++ interoperability
* CMake-based builds

---

## Architecture

The library follows an **M:N threading model**:

```text
                    User Threads
              ┌─────────────────────┐
              │ UThread 1            │
              │ UThread 2            │
              │ UThread 3            │
              │ ...                  │
              │ UThread N            │
              └──────────┬──────────┘
                         │
                    User Scheduler
                         │
              ┌──────────┴──────────┐
              │                     │
        Kernel Worker 1       Kernel Worker M
              │                     │
        ┌─────┴─────┐         ┌─────┴─────┐
        │ CPU/Core  │         │ CPU/Core  │
        └───────────┘         └───────────┘
```

`N` user-level threads are scheduled over `M` kernel-level worker threads.

For example:

```text
N = 100 user threads
M = 4 kernel workers
```

The scheduler can execute the 100 user threads using only 4 kernel workers.

---

## Project Structure

```text
mn_uthread/
│
├── CMakeLists.txt
│
├── README.md
│
├── include/
│   ├── public/
│   │   └── uthread.h
│   │
│   └── private/
│       └── ...
│
├── src/
│   ├── uthread.c
│   ├── context.S
│   └── main.c
│
├── tests/
│   └── mpmc.cpp
│
└── build/
    └── ...
```

### Important files

#### `src/uthread.c`

Contains the core user-level threading implementation, including:

* Thread creation
* Thread scheduling
* Thread state management
* `yield`
* `join`
* Worker management
* Synchronization
* User thread stacks

#### `src/context.S`

Contains the low-level x86-64 context switching implementation.

The context switch saves/restores registers such as:

```text
RSP
RIP
RBP
RBX
R12
R13
R14
R15
```

This allows execution to move from one user thread to another without using an OS-level thread context switch.

#### `src/main.c`

Example program used to test the user-level threading implementation.

#### `tests/mpmc.cpp`

C++ test for the MPMC / multi-producer multi-consumer functionality.

---

# Requirements

The project currently targets:

* **Linux**
* **x86-64**
* GCC / Clang
* C11
* C++20
* CMake >= 3.16
* POSIX threads
* GNU/Linux APIs

Check your compiler:

```bash
gcc --version
g++ --version
cmake --version
```

Check your architecture:

```bash
uname -m
```

Expected:

```text
x86_64
```

Check the operating system:

```bash
uname -s
```

Expected:

```text
Linux
```

---

# Build

## 1. Clone the repository

```bash
git clone <repository-url>
cd mn_uthread
```

---

## 2. Configure the Debug build

For development and debugging:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug
```

This enables debug symbols and disables most compiler optimizations.

---

## 3. Build

```bash
cmake --build build -j$(nproc)
```

For example, on a machine with 16 CPUs:

```bash
cmake --build build -j16
```

---

# Running the Tests

The project uses **CTest**.

Run all tests:

```bash
ctest --test-dir build
```

For more useful output:

```bash
ctest --test-dir build --output-on-failure
```

You should see tests similar to:

```text
Test project .../mn_uthread/build

    Start 1: yield_join
1/2 Test #1: yield_join ............... Passed

    Start 2: mpmc
2/2 Test #2: mpmc ..................... Passed

100% tests passed
```

---

# Running Individual Tests

## Yield / Join test

```bash
./build/test_yield_join
```

## MPMC test

```bash
./build/test_mp_mc
```

---

# Release Build

For optimized execution:

```bash
cmake -S . -B build-release \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-release -j$(nproc)
```

Run:

```bash
ctest --test-dir build-release --output-on-failure
```

---

# RelWithDebInfo Build

For low-level debugging where you want both optimization and debug symbols:

```bash
cmake -S . -B build-relwithdebinfo \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build build-relwithdebinfo -j$(nproc)
```

This configuration is particularly useful when debugging scheduler and context-switching behaviour.

---

# Debugging with GDB

Build the Debug configuration:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build build -j$(nproc)
```

Start GDB:

```bash
gdb ./build/test_yield_join
```

Set a breakpoint:

```gdb
break main
```

Run:

```gdb
run
```

Useful commands:

```gdb
next
step
continue
info threads
bt
info registers
```

For assembly-level debugging:

```gdb
disassemble context_switch
```

You can inspect registers using:

```gdb
info registers
```

For example:

```gdb
info registers rsp
info registers rip
info registers rbp
```

---

# Context Switching

The user-level context is represented by a structure containing the registers required to resume execution.

Conceptually:

```text
struct context
{
    rsp
    rip
    rbp
    rbx
    r12
    r13
    r14
    r15
};
```

The assembly routine performs approximately:

```text
context_switch(old, new)
        │
        ├── save current stack pointer
        ├── save callee-saved registers
        ├── save return instruction pointer
        │
        ├── load new stack pointer
        ├── restore new registers
        │
        └── resume new thread
```

This is the core mechanism behind the user-level scheduler.

---

# User Thread Lifecycle

A user thread generally moves through states such as:

```text
             ┌─────────┐
             │  READY  │
             └────┬────┘
                  │
                  ▼
             ┌─────────┐
             │ RUNNING │
             └────┬────┘
              │   │   │
          yield   │   │
              │   │   │
              ▼   │   ▼
           READY  │  BLOCKED
                  │
                  ▼
                DONE
```

A thread can:

1. Start in `READY`
2. Be selected by the scheduler
3. Transition to `RUNNING`
4. Call `yield()`
5. Become `READY` again
6. Block on synchronization
7. Eventually transition to `DONE`
8. Be joined by another thread

---

# M:N Scheduling

The project separates:

```text
User-level threads
```

from:

```text
Kernel-level worker threads
```

For example:

```text
User threads:

U1 U2 U3 U4 U5 U6 U7 U8


Kernel workers:

W1 W2 W3
```

The scheduler maps the user threads onto the available workers:

```text
          User threads
      ┌─────────────────┐
      │ U1 U2 U3 U4 U5  │
      │ U6 U7 U8        │
      └────────┬────────┘
               │
          Scheduler
        ┌──────┼──────┐
        ▼      ▼      ▼
       W1     W2     W3
```

This allows many user threads to execute using a smaller number of kernel workers.

---

# Linux / x86-64 Requirement

This implementation depends on Linux and x86-64 specific functionality.

The CMake configuration checks:

```text
Operating System: Linux
Architecture:     x86-64
```

The project uses x86-64 registers such as:

```text
RSP
RIP
RBP
RBX
R12
R13
R14
R15
```

Therefore, the current implementation is **not portable to ARM64, macOS, Windows, or 32-bit x86**.

Porting the project to another architecture would require a new context-switch implementation.

---

# Compiler Configuration

The project uses:

```text
C   : C11
C++ : C++20
ASM : GNU assembler / x86-64
```

Common warnings:

```text
-Wall
-Wextra
-Wpedantic
```

The project also uses:

```text
-fno-omit-frame-pointer
```

This is useful when debugging stack and context-switching problems.

---

# Threading Support

The project uses CMake's thread abstraction:

```cmake
find_package(Threads REQUIRED)
```

and:

```cmake
Threads::Threads
```

rather than manually passing:

```text
-pthread
```

This allows CMake to select the appropriate threading flags for the target platform.

---

# CMake Commands

Configure:

```bash
cmake -S . -B build
```

Configure Debug:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug
```

Build:

```bash
cmake --build build -j$(nproc)
```

Clean and rebuild:

```bash
rm -rf build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

---

# Compile Commands

The project enables:

```cmake
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

Therefore CMake generates:

```text
build/compile_commands.json
```

This is useful for:

* VS Code
* clangd
* static analysis
* compiler inspection
* debugging compilation issues

---

# Development Workflow

A typical development cycle is:

```bash
# Configure
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Debug
gdb ./build/test_yield_join
```

For changes to the scheduler or context switch:

```text
modify code
    ↓
cmake --build build
    ↓
run tests
    ↓
run under GDB
    ↓
inspect registers / stack
    ↓
repeat
```

---

# Design Goals

The project is primarily intended for learning and experimentation with systems programming concepts.

Key areas include:

### User-level scheduling

Scheduling threads without relying entirely on the OS scheduler.

### Context switching

Understanding how CPU registers and stacks are saved and restored.

### M:N threading

Mapping many user threads onto a smaller set of kernel workers.

### Synchronization

Understanding synchronization between user-level threads and kernel workers.

### Lock-free programming

The project also contains MPMC-related code for exploring concurrent data structures and atomic operations.

---

# Current Limitations

The implementation is intentionally low-level and experimental.

Current limitations include:

* Linux only
* x86-64 only
* Custom context switching
* Custom user-level stacks
* Limited portability
* Low-level synchronization
* Debuggers and sanitizers may require special handling around custom stacks
* Not intended to replace production threading libraries such as `pthread`

---

# Future Improvements

Potential future improvements include:

* Work-stealing scheduler
* Per-worker queues
* CPU affinity
* Better load balancing
* Thread cancellation
* Timed waits
* Condition variables
* User-level mutexes
* Improved memory management
* Custom stack allocator
* Guard pages for thread stacks
* Scheduler statistics
* Performance benchmarks
* C++ API
* Support for additional architectures

---

# License

This project is intended for educational and experimental use.

Add an appropriate license here if this project is distributed publicly.
