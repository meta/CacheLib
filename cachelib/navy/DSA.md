# Intel DSA host setup

Navy can offload checksum and copy work to the Intel Data Streaming
Accelerator (DSA) through Intel's [DTO library](https://github.com/intel/DTO).
This page describes how to prepare a host for it. CacheLib does not change
device configuration, so set up the work queues before starting the cache.

## Requirements

- Linux on x86_64 with a DSA device managed by the `idxd` driver, and
  [accel-config](https://github.com/intel/idxd-config) to configure it.
- CacheLib built with DTO support. For CMake builds, configure with
  `-DBUILD_WITH_DTO=ON`. CMake uses an installed DTO 1.2 or later, or fetches
  DTO 1.2, which needs the libaccel-config development package.
- A shared, user-type work queue that the cache process can open. DTO uses
  only shared work queues.
- `block_on_fault=1` on every work queue the cache uses.

## Block-on-fault

Navy submits DSA operations with DTO's block-on-fault flag. On a work queue
with block-on-fault enabled, a page fault in a source or destination buffer
makes the device wait while the kernel resolves it, and the operation
completes on DSA. On a work queue without it, DTO drops the flag because the
device would reject it. A page fault then stops the operation partway, and
Navy redoes the whole operation on the CPU. Results stay correct, but the
offload is lost, for example for copies into newly allocated buffers whose
pages have not been touched yet.

## Configure a work queue

These commands configure one shared work queue on an otherwise unused,
disabled `dsa0`. Adjust the device, engine, queue size, and limits for your
host, and stop any users of a work queue before reconfiguring it.

```sh
sudo accel-config config-engine dsa0/engine0.0 --group-id=0
sudo accel-config config-wq dsa0/wq0.0 \
  --group-id=0 --mode=shared --type=user --name=cachelib \
  --driver-name=user --priority=10 --wq-size=128 --threshold=127 \
  --max-batch-size=64 --max-transfer-size=67108864 --block-on-fault=1
sudo accel-config enable-device dsa0
sudo accel-config enable-wq dsa0/wq0.0
```

Set `--max-transfer-size` to at least the largest buffer Navy offloads, which
is at most the BlockCache region size. This example allows 64 MiB. DTO does
not submit larger operations, so Navy runs them on the CPU. Keep
`--threshold` below `--wq-size`, as accel-config recommends for shared work
queues.

If you start from a sample configuration instead, check `block_on_fault` and
`max_transfer_size` first. DTO's samples (`accelConfig.sh` and
`dto-4-dsa.conf` in the DTO repository) and most of the profiles that ship
with accel-config set `block_on_fault` to `0`, and many limit
`max_transfer_size` to 2 MiB or less.

The configuration does not persist across reboots, so apply it at every boot,
before the cache starts. One way is to save it once with
`accel-config save-config -s <file>`, then run
`accel-config load-config -c <file> -e` at boot, which loads the saved
configuration and enables its devices and work queues.

Give the cache process read and write access to the work queue's device file,
`/dev/dsa/wq0.0`.

## Select work queues for DTO

Set `DTO_WQ_LIST` in the cache process's environment to the work queues
configured for it:

```sh
export DTO_WQ_LIST='wq0.0'
```

Separate several work queues with semicolons (for example, `wq0.0;wq2.0`),
and configure each one with block-on-fault. If `DTO_WQ_LIST` is unset, DTO
built with accel-config support uses every enabled shared user work queue it
finds, which can include work queues configured for other applications. DTO
built without accel-config support requires `DTO_WQ_LIST`.

## Verify the configuration

Check each work queue before starting the cache:

```sh
cat /sys/bus/dsa/devices/wq0.0/state              # enabled
cat /sys/bus/dsa/devices/wq0.0/mode               # shared
cat /sys/bus/dsa/devices/wq0.0/block_on_fault     # 1
cat /sys/bus/dsa/devices/wq0.0/max_transfer_size  # at least the largest offloaded buffer
```

DTO reads the work queue configuration when the cache process starts, so
restart the cache after changing it.
