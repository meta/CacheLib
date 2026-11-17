/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use std::time::Duration;

use anyhow::Result;

use super::LruCacheConfig;
use super::RebalanceStrategy;
use super::ShrinkMonitor;
use super::ShrinkMonitorType;
use super::configure_shrinker;
use crate::ffi;

fn manual_shrinker(shrinker_type: ShrinkMonitorType) -> ShrinkMonitor {
    ShrinkMonitor {
        shrinker_type,
        interval: Duration::new(1, 0),
        max_resize_per_iteration_percent: 10,
        max_removed_percent: 90,
        strategy: RebalanceStrategy::LruTailAge {
            age_difference_ratio: 0.1,
            min_retained_slabs: 1,
        },
    }
}

fn monitor_has_empty_pool_recovery(config: LruCacheConfig) -> Result<bool> {
    let mut cache_config = ffi::make_lru_allocator_config()?;
    configure_shrinker(
        cache_config.pin_mut(),
        config.shrinker,
        config.empty_pool_recovery,
    )?;
    Ok(ffi::get_memory_monitor_empty_pool_recovery(&cache_config))
}

#[test]
fn test_empty_pool_recovery_is_off_by_default() -> Result<()> {
    let config = LruCacheConfig::new(128 * 1024 * 1024).set_shrinker(manual_shrinker(
        ShrinkMonitorType::ResidentSize {
            max_process_size_gib: 16,
            min_process_size_gib: 1,
        },
    ));
    assert!(
        !monitor_has_empty_pool_recovery(config)?,
        "the manual monitor should get empty-pool recovery off unless asked"
    );
    Ok(())
}

#[test]
fn test_empty_pool_recovery_reaches_manual_monitors() -> Result<()> {
    let shrinker_types = [
        ShrinkMonitorType::ResidentSize {
            max_process_size_gib: 16,
            min_process_size_gib: 1,
        },
        ShrinkMonitorType::FreeMemory {
            max_free_mem_gib: 16,
            min_free_mem_gib: 1,
        },
    ];
    for shrinker_type in shrinker_types {
        let config = LruCacheConfig::new(128 * 1024 * 1024)
            .set_shrinker(manual_shrinker(shrinker_type))
            .set_empty_pool_recovery(true);
        assert!(
            monitor_has_empty_pool_recovery(config)?,
            "the C++ monitor config should carry empty-pool recovery"
        );
    }
    Ok(())
}

#[test]
fn test_empty_pool_recovery_without_shrinker_configures_no_monitor() -> Result<()> {
    let config = LruCacheConfig::new(128 * 1024 * 1024).set_empty_pool_recovery(true);
    assert!(
        !monitor_has_empty_pool_recovery(config)?,
        "without a shrinker there is no monitor to configure"
    );
    Ok(())
}
