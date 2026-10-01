# Benchmarking alligator

## About mimalloc-bench

[mimalloc-bench](https://github.com/daanx/mimalloc-bench) is a suite of real programs and stress tests used to compare memory allocators. Each test is run once per allocator, and the suite records the time taken and the peak memory used (RSS).

The tests used here:

| Test | What it does |
|---|---|
| cfrac | Factors large numbers. Makes many small, short-lived allocations. |
| espresso | Logic minimisation tool. Mixed small allocations with steady reuse. |
| barnes | N-body physics simulation. Few allocations, mostly compute. |
| gs | Ghostscript rendering a large PDF file. |
| glibc-simple | glibc's own malloc test. Tight loop of small malloc/free pairs. |
| malloc-large | Allocates and frees blocks between 5 MB and 25 MB. |
| cache-scratch | Checks whether freed memory gets reused in a way that hurts CPU caches. |
| cache-thrash | Checks whether separate objects share cache lines. |
| alloc-test | Random mix of allocations of different sizes and lifetimes. |
| security | 116 small programs that each try a heap misuse (double free, overflow, invalid free, and so on) and check whether the allocator catches it. |

All of these are single-threaded workloads, or are run with one thread by passing `-j=1` (cache-scratch, cache-thrash and alloc-test).

**Not run:** larson, larson-sized, mstress, rptest, xmalloc-test, glibc-thread, mleak, rbstress and rocksdb, because they are multi-threaded and alligator is not thread-safe. redis, z3, lua, lean and linux are not run either, because they need `malloc_usable_size` or aligned allocation (`posix_memalign`, `aligned_alloc`, `memalign` and `valloc`), which alligator does not provide yet.

## Setup

### 1. Build mimalloc-bench

```bash
git clone https://github.com/daanx/mimalloc-bench
cd mimalloc-bench
./build-bench-env.sh packages mi bench
```

This installs the required packages and builds mimalloc (`mi`, `mi-sec`) and the benchmark programs.

### 2. Build and install alligator

```bash
cd alligator
make lib                                          # builds ./bin/liballigator.so
sudo install -m 755 bin/liballigator.so /usr/local/lib/
```

### 3. Add alligator to bench.sh

In `mimalloc-bench/bench.sh`, make two changes:

1. Add `al` to the list of allocators:
   ```bash
   readonly alloc_all="sys al kp dh ff ..."
   ```
2. Register its library path next to the other `alloc_lib_add` lines:
   ```bash
   alloc_lib_add "al"     "/usr/local/lib/liballigator.so"
   ```

### 4. Allow preloading for gs

On Ubuntu, the AppArmor profile for `gs` stops it from loading preloaded libraries from `/home` and `/usr/local/lib`. This blocks both alligator and the mimalloc builds. Switch the profile to complain mode while benchmarking:

```bash
sudo apt install apparmor-utils
sudo aa-complain /etc/apparmor.d/gs
```

When you are done benchmarking, turn the profile back on:

```bash
sudo aa-enforce /etc/apparmor.d/gs
```

## Running the tests

From `mimalloc-bench/out/bench`:

```bash
../../bench.sh -n=10 -j=1 al sys mi mi-sec cfrac espresso barnes gs cthrash cscratch alloc-test glibc-simple malloc-large security
```

- `-n=10` repeats each test 10 times.
- `-j=1` runs everything with one thread.
- `sys` is glibc malloc. `mi` and `mi-sec` are mimalloc and its secure build.
- `cthrash` and `cscratch` are bench.sh's names for cache-thrash and cache-scratch.

Timings and memory use are written to `benchres.csv` in the same folder. This file is overwritten on every run, so copy it before running again. Security results are printed to the terminal.

## Results

The tests ran on two machines:

- **aarch64:** Ubuntu VM (UTM) on an Apple M5 Mac, 4 cores.
- **x86-64:** Linux machine with an Intel Xeon @ 2.80 GHz.

Absolute times are not comparable between the two machines. Compare allocators within each table.

Each cell shows the median time and median peak memory (RSS) over all runs. The last two rows are the geometric mean over all 9 tests, relative to glibc (`sys` = 1.00). Lower is better.

### aarch64 (Ubuntu VM on Apple M5)

| Test | al | sys (glibc) | mi | mi-sec |
|---|---|---|---|---|
| cfrac | 2.91 s / 6.0 MB | 1.98 s / 6.0 MB | 1.90 s / 6.0 MB | 2.55 s / 6.0 MB |
| espresso | 2.76 s / 6.0 MB | 2.67 s / 6.0 MB | 2.38 s / 6.0 MB | 2.70 s / 6.0 MB |
| barnes | 1.15 s / 56.4 MB | 1.16 s / 59.5 MB | 1.19 s / 56.6 MB | 1.18 s / 56.7 MB |
| gs | 0.17 s / 35.9 MB | 0.17 s / 34.2 MB | 0.15 s / 35.0 MB | 0.16 s / 39.2 MB |
| cache-thrash1 | 0.67 s / 6.0 MB | 0.66 s / 6.0 MB | 0.67 s / 6.0 MB | 0.67 s / 6.0 MB |
| cache-scratch1 | 0.53 s / 6.0 MB | 0.52 s / 6.0 MB | 0.53 s / 6.0 MB | 0.54 s / 6.0 MB |
| alloc-test1 | 9.85 s / 16.8 MB | 2.25 s / 13.8 MB | 1.92 s / 12.6 MB | 2.64 s / 13.9 MB |
| glibc-simple | 1.75 s / 6.0 MB | 2.21 s / 6.0 MB | 1.21 s / 6.0 MB | 1.88 s / 6.0 MB |
| malloc-large | 0.32 s / 514.5 MB | 0.27 s / 521.7 MB | 0.34 s / 558.6 MB | 0.33 s / 558.6 MB |
| **Geo. mean time vs glibc** | **1.23** | 1.00 | 0.92 | 1.05 |
| **Geo. mean memory vs glibc** | **1.02** | 1.00 | 1.00 | 1.02 |

10 runs per test. Full data: [results-aarch64-vm.csv](https://github.com/niharika-khare/alligator/blob/mainline/mimalloc-bench-results/aarch64_ubuntu_VM_macOS_M5.csv).

### x86-64 (Intel Xeon)

| Test | al | sys (glibc) | mi | mi-sec |
|---|---|---|---|---|
| cfrac | 8.60 s / 3.2 MB | 5.74 s / 3.3 MB | 5.49 s / 3.3 MB | 6.79 s / 3.8 MB |
| espresso | 7.29 s / 2.3 MB | 7.08 s / 2.6 MB | 6.28 s / 3.4 MB | 7.32 s / 5.5 MB |
| barnes | 2.91 s / 57.1 MB | 2.94 s / 57.1 MB | 2.89 s / 57.1 MB | 2.90 s / 57.4 MB |
| gs | 0.41 s / 35.8 MB | 0.49 s / 33.0 MB | 0.41 s / 33.4 MB | 0.39 s / 37.9 MB |
| cache-thrash1 | 1.65 s / 3.9 MB | 1.62 s / 4.0 MB | 1.73 s / 4.2 MB | 1.62 s / 4.2 MB |
| cache-scratch1 | 1.73 s / 4.0 MB | 1.77 s / 3.9 MB | 1.69 s / 4.2 MB | 1.71 s / 4.2 MB |
| alloc-test1 | 26.50 s / 17.3 MB | 5.54 s / 14.0 MB | 4.26 s / 13.4 MB | 6.28 s / 14.5 MB |
| glibc-simple | 3.83 s / 2.0 MB | 4.36 s / 2.0 MB | 2.27 s / 2.1 MB | 3.67 s / 2.4 MB |
| malloc-large | 4.12 s / 515.0 MB | 4.79 s / 522.0 MB | 3.97 s / 559.2 MB | 4.29 s / 559.1 MB |
| **Geo. mean time vs glibc** | **1.18** | 1.00 | 0.85 | 0.97 |
| **Geo. mean memory vs glibc** | **1.02** | 1.00 | 1.06 | 1.18 |

10 runs per test, except gs and alloc-test1, which have 5 runs. These timings were taken with an earlier build of alligator, before the alignment and size-limit changes. Full data: [results-x86-64.csv](https://github.com/niharika-khare/alligator/blob/mainline/mimalloc-bench-results/x86-64_Intel_Linux_Cloud_VM.csv).

### Security

Number of the 116 security checks each allocator catches. Higher is better.

| Machine | al | sys (glibc) | mi | mi-sec | Output |
|---|---|---|---|---|---|
| aarch64 | **69** | 69 | 32 | 53 | [security-aarch64-vm.txt](https://github.com/niharika-khare/alligator/blob/mainline/mimalloc-bench-results/aarch64_security_results.txt) |
| x86-64 | **64** | 68 | 32 | 50 | [security-x86-64.txt](https://github.com/niharika-khare/alligator/blob/mainline/mimalloc-bench-results/x86-64_security_results.txt) |

- al uses the latest code, built with `make lib` (`-O3`).
- **On aarch64, al catches as many checks as glibc (69 each). On x86-64 it catches 4 fewer than glibc.** On both machines it catches about twice as many as mimalloc, and more than mimalloc's secure build.
- al catches every double free and invalid free test.
- The 5 checks al catches on aarch64 but misses on x86-64 are small and medium buffer overflows (1 byte and 32 bytes).
- al does not catch use-after-free or zero-size checks, or most reuse and underflow checks.
- On x86-64, one mi-sec check (`zero_on_malloc_large`) timed out and is counted as not caught.

### Summary: alligator vs glibc

| | aarch64 | x86-64 |
|---|---|---|
| Speed (% of glibc) | **82%** | **85%** |
| Memory (peak RSS vs glibc) | **2% more** | **2% more** |

- **Faster than glibc:** glibc-simple on both machines, and gs and malloc-large on x86-64.
- **About the same as glibc:** barnes, espresso, cache-thrash and cache-scratch.
- **Slower than glibc:** cfrac (about 1.5× the time) and alloc-test1 (about 4.4–4.8× the time). alloc-test1 pulls the average down the most.
- **Memory:** within a few percent of glibc on every test except alloc-test1, which uses about 22% more.
