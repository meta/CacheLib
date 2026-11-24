# BlockCache checksum offload example

This synthetic workload uses 64–128 KiB values, above the 16 KiB offload
threshold. Its working set exceeds the DRAM cache, so it exercises a
memory-backed Navy BlockCache without using an SSD or shared memory.
Use it to compare checksum execution; it does not measure SSD performance.

Build CacheBench with DTO and configure an accessible DSA work queue. See the
[Navy DSA guide](../../../navy/DSA.md) for prerequisites and queue setup.

From the CacheLib repository root:

```sh
cmake -S cachelib -B build-cachelib -DBUILD_WITH_DTO=ON
cmake --build build-cachelib --target cachebench
./build-cachelib/cachebench/cachebench \
  --json_test_config=cachelib/cachebench/test_configs/dsa/checksum_offload.json
```

For the CPU baseline, copy the config and change only `navyChecksumOffload`
to `false`. Both runs use CRC32C and the same workload.

The config sets `printNvmCounters`, so both runs print the Navy counters. In
the DSA run, `navy_bc_copy_crc_offloaded_ops` (writes) and
`navy_bc_crc_offloaded_ops` (verification) count operations that completed on
DSA; the matching `*_fallback_ops` counters count operations redone on the
CPU. These counters are absent if offload was disabled at startup; the log
says why. Check that both runs report Navy reads and writes before comparing.
