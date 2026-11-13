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

#include "cachelib/allocator/ChainedHashTable.h"
#include "cachelib/allocator/MM2Q.h"
#include "cachelib/allocator/MMLru.h"
#include "cachelib/allocator/MMS3FIFO.h"
#include "cachelib/allocator/MMTinyLFU.h"
#include "cachelib/allocator/MMWTinyLFU.h"
namespace facebook::cachelib {
// Types of AccessContainer and MMContainer
// MMType
const int MMLru::kId = 1;
const int MM2Q::kId = 2;
const int MMTinyLFU::kId = 3;
const int MMWTinyLFU::kId = 4;
const int MMS3FIFO::kId = 5;

// AccessType
const int ChainedHashTable::kId = 1;
} // namespace facebook::cachelib
