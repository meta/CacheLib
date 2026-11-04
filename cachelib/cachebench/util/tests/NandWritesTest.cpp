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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cachelib/cachebench/util/NandWrites.h"
#include "folly/String.h"
#include "folly/Subprocess.h"

using namespace testing;

namespace facebook {
namespace hw {

class MockProcess : public Process {
 public:
  MOCK_METHOD0(communicate, std::pair<std::string, std::string>());
  MOCK_METHOD0(wait, folly::ProcessReturnCode());

  static std::shared_ptr<MockProcess> createWithOutput(
      const std::string& stdout) {
    auto mockProcess = std::make_shared<MockProcess>();
    EXPECT_CALL(*mockProcess, communicate())
        .WillOnce(Return(std::make_pair(stdout, "")));
    EXPECT_CALL(*mockProcess, wait())
        .WillOnce(Return(folly::ProcessReturnCode::make(0)));
    return mockProcess;
  }
};

class MockProcessFactory : public ProcessFactory {
 public:
  MOCK_CONST_METHOD4(createProcess,
                     std::shared_ptr<Process>(const std::vector<std::string>&,
                                              const folly::Subprocess::Options&,
                                              const char*,
                                              const std::vector<std::string>*));

  struct ExpectedCmd {
    std::vector<std::string> expectedArgs;
    std::string stdout;
  };

  void expectedCommands(const std::vector<ExpectedCmd>& expectedCmds) {
    for (const auto& cmd : expectedCmds) {
      std::shared_ptr<MockProcess> proc =
          MockProcess::createWithOutput(cmd.stdout);
      std::cout << "Setting expectation: " << folly::join(",", cmd.expectedArgs)
                << std::endl;
      EXPECT_CALL(*this, createProcess(cmd.expectedArgs, _, _, _))
          .WillOnce(Return(proc));
    }
  }
};

class NandWritesTest : public Test {
 public:
  void SetUp() override {
    mockFactory_ = std::make_shared<MockProcessFactory>();
  }

  void TearDown() override {}

  std::shared_ptr<MockProcessFactory> mockFactory_;
};

constexpr auto& kNvmePath = "/usr/local/sbin/nvme";

// If we fail to run the nvme command, nandWriteBytes should throw an
// invalid_argument.
TEST_F(NandWritesTest, nandWriteBytes_throwsOnSubprocessSpawnError) {
  EXPECT_CALL(*mockFactory_, createProcess(_, _, _, _))
      .WillOnce(Throw(folly::SubprocessSpawnError(kNvmePath, 127, ENOENT)));
  EXPECT_THROW(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_),
               std::invalid_argument);
}

// If the `nvme list` command doesn't return valid JSON, we should throw an
// invalid_argument.
TEST_F(NandWritesTest, nandWriteBytes_throwsIfListResponseNotValidJson) {
  auto listCmd = MockProcess::createWithOutput("not valid json");
  EXPECT_CALL(*mockFactory_,
              createProcess(
                  ElementsAre(kNvmePath, "list", "-o", "json", "-v"), _, _, _))
      .WillOnce(Return(listCmd));
  EXPECT_THROW(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_),
               std::invalid_argument);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesSamsungDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "S41RNF0M207885",
              "ModelNumber" : "SAMSUNG MZ1LB1T9HALS-000FB",
              "Firmware" : "EDA78F2Q",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 1880372846592,
                  "MaximumLBA" : 459076086,
                  "PhysicalSize" : 1880375648256,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
[015:000] PhysicallyWrittenBytes                            : 5357954930143232
[031:016] Physically Read Bytes                             : 7095487750725632
[037:032] Bad NAND Block Count (Raw Value)                  : 0
[039:038] Bad NAND Block Count (Normalized Value)           : 100
[047:040] Uncorrectable Read Error Count                    : 0
[055:048] Soft ECC Error Count                              : 0
[059:056] SSD End to end Correction Count (Detected Errors) : 0
[063:060] SSD End to end Correction Count (Corrected Errors): 0
[064:064] System Data Percentage Used                       : 1
[068:065] User Data Erase Count (Min)                       : 2291
[072:069] User Data Erase Count (Max)                       : 2497
[080:073] Refresh Count                                     : 23063
[086:081] Program Fail Count (Raw Value)                    : 0
[088:087] Program Fail Count (Normalized Value)             : 100
[094:089] User Data Erase Fail Count (Raw Value)            : 0
[096:095] User Data Erase Fail Count (Normalized Value)     : 100
[102:097] System Area Erase Fail Count (Raw Value)          : 0
[104:103] System Area Erase Fail Count (Normalized value)   : 100
[105:105] Thermal Throttling Status                         : 0
[106:106] Thermal Throttling Count                          : 0
[108:107] PHY Error Count                                   : 16
[110:109] Bad DLLP Count                                    : 0
[112:111] Bad TLP Count                                     : 0
[114:113] Reserved                                          : 0
[118:115] Incomplete Shutdowns                              : 0
[119:119] % Free Blocks                                     : 0
[121:120] PCIe Correctable Error Count (RTS)                : 0
[123:122] PCIe Correctable Error Count (RRS)                : 0
[131:124] XOR Recovery Count                                : 0
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "samsung", "vs-smart-add-log", "/dev/nvme0n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_),
            5357954930143232);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesSamsungPM983aDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "200405805037",
              "ModelNumber" : "WDC CL SN720 SDAQNTW-512G-1020",
              "Firmware" : "10105120",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 512110190592,
                  "MaximumLBA" : 1000215216,
                  "PhysicalSize" : 512110190592,
                  "SectorSize" : 512
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme1",
              "SerialNumber" : "S5XBNE0N300490",
              "ModelNumber" : "MZ1LB960HBJR-000FB",
              "Firmware" : "EDW73F2Q",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme1n1",
                  "NSID" : 1,
                  "UsedBytes" : 497998016512,
                  "MaximumLBA" : 122096646,
                  "PhysicalSize" : 500107862016,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme2",
              "SerialNumber" : "S5XBNE0N300481",
              "ModelNumber" : "MZ1LB960HBJR-000FB",
              "Firmware" : "EDW73F2Q",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme2n1",
                  "NSID" : 1,
                  "UsedBytes" : 498003058688,
                  "MaximumLBA" : 122096646,
                  "PhysicalSize" : 500107862016,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
[015:000] PhysicallyWrittenBytes                            : 35061362294784
[031:016] Physically Read Bytes                             : 82979098025984
[037:032] Bad NAND Block Count (Raw Value)                  : 0
[039:038] Bad NAND Block Count (Normalized Value)           : 100
[047:040] Uncorrectable Read Error Count                    : 0
[055:048] Soft ECC Error Count                              : 0
[059:056] SSD End to end Correction Count (Detected Errors) : 0
[063:060] SSD End to end Correction Count (Corrected Errors): 0
[064:064] System Data Percentage Used                       : 0
[068:065] User Data Erase Count (Min)                       : 9
[072:069] User Data Erase Count (Max)                       : 42
[080:073] Refresh Count                                     : 0
[086:081] Program Fail Count (Raw Value)                    : 0
[088:087] Program Fail Count (Normalized Value)             : 100
[094:089] User Data Erase Fail Count (Raw Value)            : 0
[096:095] User Data Erase Fail Count (Normalized Value)     : 100
[102:097] System Area Erase Fail Count (Raw Value)          : 0
[104:103] System Area Erase Fail Count (Normalized value)   : 100
[105:105] Thermal Throttling Status                         : 0
[106:106] Thermal Throttling Count                          : 0
[108:107] PHY Error Count                                   : 0
[110:109] Bad DLLP Count                                    : 0
[112:111] Bad TLP Count                                     : 0
[114:113] Reserved                                          : 0
[118:115] Incomplete Shutdowns                              : 0
[119:119] % Free Blocks                                     : 1
[121:120] PCIe Correctable Error Count (RTS)                : 0
[123:122] PCIe Correctable Error Count (RRS)                : 0
[131:124] XOR Recovery Count                                : 0
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "samsung", "vs-smart-add-log", "/dev/nvme1n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme1n1", kNvmePath, mockFactory_), 35061362294784);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesSamsungPM9A3Device) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "        21062E6B8061",
              "ModelNumber" : "MTFDHBA512TCK",
              "Firmware" : "P1FB007",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 512110190592,
                  "MaximumLBA" : 1000215216,
                  "PhysicalSize" : 512110190592,
                  "SectorSize" : 512
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme1",
              "SerialNumber" : "S5X9NG0T116005",
              "ModelNumber" : "MZOL23T8HCLS-00AFB",
              "Firmware" : "GDA82F2Q",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme1n1",
                  "NSID" : 1,
                  "UsedBytes" : 104910848,
                  "MaximumLBA" : 918149526,
                  "PhysicalSize" : 3760740458496,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme2",
              "SerialNumber" : "S5X9NG0T116027",
              "ModelNumber" : "MZOL23T8HCLS-00AFB",
              "Firmware" : "GDA82F2Q",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme2n1",
                  "NSID" : 1,
                  "UsedBytes" : 0,
                  "MaximumLBA" : 918149526,
                  "PhysicalSize" : 3760740458496,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
[015:000] PhysicallyWrittenBytes                            : 241393664
[031:016] Physically Read Bytes                             : 106217472
[037:032] Bad NAND Block Count (Raw Value)                  : 0
[039:038] Bad NAND Block Count (Normalized Value)           : 100
[047:040] Uncorrectable Read Error Count                    : 0
[055:048] Soft ECC Error Count                              : 0
[059:056] SSD End to end Correction Count (Detected Errors) : 0
[063:060] SSD End to end Correction Count (Corrected Errors): 0
[064:064] System Data Percentage Used                       : 0
[068:065] User Data Erase Count (Min)                       : 0
[072:069] User Data Erase Count (Max)                       : 1
[080:073] Refresh Count                                     : 0
[086:081] Program Fail Count (Raw Value)                    : 0
[088:087] Program Fail Count (Normalized Value)             : 100
[094:089] User Data Erase Fail Count (Raw Value)            : 0
[096:095] User Data Erase Fail Count (Normalized Value)     : 100
[102:097] System Area Erase Fail Count (Raw Value)          : 0
[104:103] System Area Erase Fail Count (Normalized value)   : 100
[105:105] Thermal Throttling Status                         : 0
[106:106] Thermal Throttling Count                          : 0
[108:107] PHY Error Count                                   : 0
[110:109] Bad DLLP Count                                    : 0
[112:111] Bad TLP Count                                     : 0
[114:113] Reserved                                          : 0
[118:115] Incomplete Shutdowns                              : 0
[119:119] % Free Blocks                                     : 96
[121:120] PCIe Correctable Error Count (RTS)                : 0
[123:122] PCIe Correctable Error Count (RRS)                : 0
[131:124] XOR Recovery Count                                : 0
[137:132] Bad System NAND block count (Raw Value)           : 0
[139:138] Bad System NAND block count (Normalized Value)    : 100
[141:140] Capacitor Health                                  : 163
[157:142] Endurance Estimate                                : 28862181
[165:158] Security Version Number                           : 4294967296
[167:166] Log Page Version                                  : 1
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "samsung", "vs-smart-add-log", "/dev/nvme1n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme1n1", kNvmePath, mockFactory_), 241393664);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesSeagateDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "HE6016KT",
              "ModelNumber" : "XM1441-1AB112048",
              "Firmware" : "PA00FB0C",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 1920383410176,
                  "MaximumLBA" : 468843606,
                  "PhysicalSize" : 1920383410176,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
Seagate Extended SMART Information :
Description                             Ext-Smart-Id    Ext-Smart-Value
--------------------------------------------------------------------------------
Bad NAND block count                    40              0x0000000000000000
Uncorrectable Read Error Count          188             0x0000000000000000
Soft ECC error count                    1               0x0000000000000000
SSD End to end correction counts        41              0x0000000000000000
System data % used                      173             0x0000000000000045
User data erase counts                  42              0x00000b4800000a0c
Refresh count                           43              0x0000000000000000
Program fail count                      171             0x0064000000000000
User data erase fail count              44              0x0064000000000000
System area erase fail count            45              0x0064000000000000
Thermal throttling status and count     46              0x0000000000000000
PCIe Correctable Error count            47              0x0000000000000000
PCIe Uncorrectable Error count          48              0x0000000000000c47
Incomplete shutdowns                    49              0x0000000000000000
Retired block count                     170             0x0000000000000f29
Wear range delta                        177             0x0000000000000003
Max lifetime temperature                194             0x0000000000000148
Max lifetime SOC temperature            253             0x0000000000000161
Remaining SSD life                      231             0x0000000000000043
Raw Read Error Count                    13              0x0000000000000000
Flash GB erased                         25956           0x000000000000000000000002d3d1e408
Physical (NAND) bytes written           60137           0x000000000000000000000002d0cdc01b
Physical (HOST) bytes written           62193           0x000000000000000000000000efb8d568
Physical (NAND) bytes read              62707           0x000000000000000000000001c728e70c
Trim count                              64506           0x00000000000000000000000000000000
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "seagate", "vs-smart-add-log", "/dev/nvme0n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_),
            6191656744448000);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesWesternDigitalDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme1",
              "SerialNumber" : "44203690005610",
              "ModelNumber" : "WUS4BB019D4M9E7",
              "Firmware" : "R9109005",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme1n1",
                  "NSID" : 1,
                  "UsedBytes" : 477467774976,
                  "MaximumLBA" : 122096646,
                  "PhysicalSize" : 500107862016,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
  SMART Cloud Attributes :-
  Physical media units written     	      	: 0 609651339018240
  Physical media units read      	      	: 0 567611790770176
  Bad user nand blocks Raw			: 0
  Bad user nand blocks Normalized		: 100
  Bad system nand blocks Raw			: 0
  Bad system nand blocks Normalized		: 100
  XOR recovery count			  	: 0
  Uncorrectable read error count		: 0
  Soft ecc error count				: 0
  End to end corrected errors			: 0
  End to end detected errors			: 0
  System data percent used			: 0
  Refresh counts				: 4
  Max User data erase counts			: 293
  Min User data erase counts			: 197
  Number of Thermal throttling events		: 0
  Current throttling status			: 0x0
  PCIe correctable error count			: 0
  Incomplete shutdowns				: 0
  Percent free blocks				: 1
  Capacitor health				: 114
  Unaligned I/O				: 0
  Security Version Number			: 0
  NUSE Namespace utilization			: 0
  PLP start count				: 42
  Endurance estimate				: 16896000000000000
  Log page version				: 2
  Log page GUID				: 0x0xafd514c97c6f4f9ca4f2bfea2810afc5
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "wdc", "vs-smart-add-log", "/dev/nvme1n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme1n1", kNvmePath, mockFactory_),
            609651339018240);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesToshibaDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "69DS10OHT7RQ",
              "ModelNumber" : "KXD51LN11T92 TOSHIBA",
              "Firmware" : "1CET6105",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 1920383078400,
                  "MaximumLBA" : 468843606,
                  "PhysicalSize" : 1920383410176,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
Vendor Log Page 0xCA for NVME device:nvme0 namespace-id:ffffffff
Total data written to NAND               : 1133997.9 GiB
Total data read from NAND                : 1463243.5 GiB
Bad NAND Block Count (Normalised)        : 100
Bad NAND Block Count (Raw)               : 0
Uncorrectable Read Error Count           : 0
Soft ECC Error Count                     : 0
End-to-end Error Count (Detected)        : 0
End-to-end Error Count (Corrected)       : 0
System Data Used                         : 8 %
User Data Erase Count (Max)              : 570
User Data Erase Count (Min)              : 470
Refresh Count                            : 26
Program Fail Count (Normalised)          : 100
Program Fail Count (Raw)                 : 0
User Data Erase Fail Count (Normalised)  : 100
User Data Erase Fail Count (Raw)         : 0
System Area Erase Fail Count (Normalised): 100
System Area Erase Fail Count (Raw)       : 0
Thermal Throttling Status                : 0
Thermal Throttling Count                 : 0
PCIe Correctable Error Count             : 0
Incomplete Shutdowns                     : 0
Free blocks                              : 13 %
Light Throttle Time (seconds)            : 0
Heavy Throttle Time (seconds)            : 0
Custom Temperature Range Time (seconds)  : 0
NAND Over-temperature Count              : 0 %
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "toshiba", "vs-smart-add-log", "/dev/nvme0n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_),
            1217620007190528);
}
TEST_F(NandWritesTest, nandWriteBytes_handlesLiteonDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "69DS10OHT7RQ",
              "ModelNumber" : "KXD51LN11T92 TOSHIBA",
              "Firmware" : "1CET6105",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 1920383078400,
                  "MaximumLBA" : 468843606,
                  "PhysicalSize" : 1920383410176,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme1",
              "SerialNumber" : "002303560014FA0B",
              "ModelNumber" : "SSSTC EPX-KW960",
              "Firmware" : "CMWF2P3",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme1n1",
                  "NSID" : 1,
                  "UsedBytes" : 498103652352,
                  "MaximumLBA" : 122096646,
                  "PhysicalSize" : 500107862016,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme2",
              "SerialNumber" : "002303560014FA0C",
              "ModelNumber" : "SSSTC EPX-KW960",
              "Firmware" : "CMWF2P3",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme2n1",
                  "NSID" : 1,
                  "UsedBytes" : 498110795776,
                  "MaximumLBA" : 122096646,
                  "PhysicalSize" : 500107862016,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
Physical(NAND) bytes written                  : 157,035,510,104,064
Physical(NAND) bytes read                     : 158,313,163,558,912
Bad NAND block count(normalized)              : 0
Bad NAND block count(raw)                     : 0
XOR Recovery count                            : 0
Uncorrectable read error count                : 0
Soft ECC error count                          : 0
SSD End to end detected errors counts         : 0
SSD End to end corrected errors counts        : 0
System data % used                            : 0
User data erase counts maximum                : 142
User data erase counts minimum                : 109
Refresh count                                 : 0
Program fail count(normalized)                : 100
Program fail count(raw)                       : 0
User data erase fail count(normalized)        : 100
User data erase fail count(raw)               : 0
System area erase fail count(normalized)      : 100
System area erase fail count(raw)             : 0
Thermal throttling status                     : 0
Thermal throttling count                      : 0
PCIe Correctable Error count                  : 53
Incomplete shutdowns                          : 0
% Free Blocks                                 : 100
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "liteon", "vs-smart-add-log", "/dev/nvme1n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme1n1", kNvmePath, mockFactory_),
            157035510104064);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesSkhmsDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "CN08Q749810308S3J",
              "ModelNumber" : "HFS512GDE9X083N",
              "Firmware" : "41021C20",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 512110190592,
                  "MaximumLBA" : 500118192,
                  "PhysicalSize" : 512110190592,
                  "SectorSize" : 512
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme1",
              "SerialNumber" : "CN08Q749810308S3J",
              "ModelNumber" : "HFS512GDE9X083N",
              "Firmware" : "41021C20",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme1n1",
                  "NSID" : 1,
                  "UsedBytes" : 497998016512,
                  "MaximumLBA" : 500118192,
                  "PhysicalSize" : 512110190592,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        },
        {
          "Controllers" : [
            {
              "Controller" : "nvme2",
              "SerialNumber" : "CN08Q749810308S3J",
              "ModelNumber" : "HFS512GDE9X083N",
              "Firmware" : "41021C20",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme2n1",
                  "NSID" : 1,
                  "UsedBytes" : 498003058688,
                  "MaximumLBA" : 500118192,
                  "PhysicalSize" : 512110190592,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
Physical Media Units Written - TLC                                          : 484533969
Physical Media Units Written - SLC                                          : 12271117
Bad User NAND Block Count (Normalized)                                      : 100
Bad User NAND Block Count (Raw)                                             : 0
XOR Recovery Count                                                          : 0
Uncorrectable Read error count                                              : 0
SSD End to End correction counts (Corrected Errors)                         : 0
SSD End to End correction counts (Detected Errors)                          : 0
SSD End to End correction counts (Uncorrected E2E Errors)                   : 0
System data %% life-used                                                    : 1%
User data erase counts (Minimum TLC)                                        : 433
User data erase counts (Maximum TLC)                                        : 484
User data erase counts (Minimum SLC)                                        : 68
User data erase counts (Maximum SLC)                                        : 78
Program fail count (Normalized)                                             : 100
Program fail count (Raw)                                                    : 0
Erase Fail Count (Normalized)                                               : 100
Erase Fail Count (Raw)                                                      : 0
PCIe Correctable Error count                                                : 1
%% Free Blocks (User)                                                       : 68%
Security Version Number                                                     : 1
%% Free Blocks (System)                                                     : 53%
TRIM Completions (Data Set Management Commands count)                       : 19776308
TRIM Completions (Incomplete TRIM commands (MBs))                           : 0
TRIM Completions (Completed TRIM (%%))                                      : 100%
Background Back-Pressure Gauge                                              : 0
Soft ECC Error Count                                                        : 0
Refresh count                                                               : 0
Bad System NAND Block Count (Normalized)                                    : 100
Bad System NAND Block Count (Raw)                                           : 0
Endurance Estimate                                                          : 300000
Thermal Throttling Status & Count (Number of thermal throttling events)     : 0
Thermal Throttling Status & Count (Current Throttling Status)               : 0x00
Unaligned I/O                                                               : 0
Physical Media Units Read                                                   : 518215267
Log Page Version                                                            : 3
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "skhms", "vs-nand-stats", "/dev/nvme0n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_), 484533969);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesIntelDevice) {
  constexpr auto& kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "CVMD7426005S1P6SGN",
              "ModelNumber" : "INTEL SSDPEDME016T4F",
              "Firmware" : "8DV1FF05",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 1600321314816,
                  "MaximumLBA" : 390703446,
                  "PhysicalSize" : 1600321314816,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]
})EOF";

  constexpr auto& kSmartLogOutput = R"EOF(
Additional Smart Log for NVME device:nvme0 namespace-id:ffffffff
key                               normalized raw
program_fail_count              : 100%       0
erase_fail_count                : 100%       0
wear_leveling                   :  70%       min: 3523, max: 3603, avg: 3562
end_to_end_error_detection_count: 100%       0
crc_error_count                 : 100%       0
timed_workload_media_wear       : 100%       63.999%
timed_workload_host_reads       : 100%       65535%
timed_workload_timer            : 100%       65535 min
thermal_throttle_status         : 100%       0%, cnt: 0
retry_buffer_overflow_count     : 100%       0
pll_lock_loss_count             : 100%       0
nand_bytes_written              : 100%       sectors: 224943088
host_bytes_written              : 100%       sectors: 172365270
)EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "intel", "smart-log-add", "/dev/nvme0n1"},
        kSmartLogOutput}});
  EXPECT_EQ(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_),
            7547837550166016);
}

TEST_F(NandWritesTest, nandWriteBytes_handlesOcpSupportedDevice_ModelListed) {
  // This test is for the case for the device where the model number is listed
  // as vendor specific command list, but only supports OCP smart add log. In
  // this case, device will throw error with the vendor specific plug-in command
  // and only return the proper result with the OCP smart-add-log command
  constexpr auto* kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "232439E0007010",
              "ModelNumber" : "WUS6A7619PJP8X7",
              "Firmware" : "RG109W08",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 486186942464,
                  "MaximumLBA" : 468843606,
                  "PhysicalSize" : 1920383410176,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]})EOF";

  constexpr auto& kSmartLogOutputErr = R"EOF(
  ERROR : WDC: unsupported device for this command
  )EOF";

  constexpr auto& kSmartLogOutputOcp = R"EOF(
  SMART Cloud Attributes :- 
  Physical media units written -   	        0 80410014056448
  Physical media units read    - 	        0 84923571298304
  Bad user nand blocks - Raw			0
  Bad user nand blocks - Normalized		100
  Bad system nand blocks - Raw			0
  Bad system nand blocks - Normalized		100
  XOR recovery count				0
  Uncorrectable read error count		0
  Soft ecc error count				0
  End to end corrected errors			0
  End to end detected errors			0
  System data percent used			0
  Refresh counts				79503
  Max User data erase counts			34
  Min User data erase counts			25
  Number of Thermal throttling events		0
  Current throttling status		  	0x0
  PCIe correctable error count			0
  Incomplete shutdowns				0
  Percent free blocks				100
  Capacitor health				100
  Unaligned I/O					0
  Security Version Number			1
  NUSE - Namespace utilization			118697984
  PLP start count				101
  Endurance estimate				23175501818990592
  Log page version				3
  Log page GUID					0xafd514c97c6f4f9ca4f2bfea2810afc5
  Errata Version Field                          0
  Point Version Field                           0
  Minor Version Field                           0
  Major Version Field                           2
  NVMe Errata Version				98
  PCIe Link Retraining Count			0
  )EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "wdc", "vs-smart-add-log", "/dev/nvme0n1"},
        kSmartLogOutputErr},
       {{kNvmePath, "ocp", "smart-add-log", "/dev/nvme0n1"},
        kSmartLogOutputOcp}});
  EXPECT_EQ(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_), 80410014056448);
}

TEST_F(NandWritesTest,
       nandWriteBytes_handlesOcpSupportedDevice_ModelNotListed) {
  // This test is for the case where the device where the model number is not
  // listed as vendor specific list, but supports OCP smart add log. In this
  // case, after it fails to find the proper vendor specific plug-in with our
  // vendor map, we will try ocp smart-add-log just in case, and it should
  // succeed.
  constexpr auto* kListOutput = R"EOF({
  "Devices" : [
    {
      "Subsystems" : [
        {
          "Controllers" : [
            {
              "Controller" : "nvme0",
              "SerialNumber" : "1424BID4T0001I020A16",
              "ModelNumber" : "HFS11111ABC",
              "Firmware" : "51096F10",
              "Transport" : "pcie",
              "Namespaces" : [
                {
                  "NameSpace" : "nvme0n1",
                  "NSID" : 1,
                  "UsedBytes" : 486186942464,
                  "MaximumLBA" : 468975616,
                  "PhysicalSize" : 1920924123136,
                  "SectorSize" : 4096
                }
              ]
            }
          ],
          "Namespaces" : []
        }
      ]
    }
  ]})EOF";

  constexpr auto& kSmartLogOutputOcp = R"EOF(
  SMART Cloud Attributes :- 
  Physical media units written -   	        0 88403820249088
  Physical media units read    - 	        0 101477049159680
  Bad user nand blocks - Raw			0
  Bad user nand blocks - Normalized		100
  Bad system nand blocks - Raw			0
  Bad system nand blocks - Normalized		100
  XOR recovery count				0
  Uncorrectable read error count		0
  Soft ecc error count				0
  End to end corrected errors			0
  End to end detected errors			0
  System data percent used			0
  Refresh counts				88867
  Max User data erase counts			37
  Min User data erase counts			28
  Number of Thermal throttling events		0
  Current throttling status		  	0x0
  PCIe correctable error count			10450
  Incomplete shutdowns				0
  Percent free blocks				100
  Capacitor health				100
  Unaligned I/O					0
  Security Version Number			1
  NUSE - Namespace utilization			118697984
  PLP start count				106
  Endurance estimate				25319737374277632
  Log page version				3
  Log page GUID					0xafd514c97c6f4f9ca4f2bfea2810afc5
  Errata Version Field                          0
  Point Version Field                           0
  Minor Version Field                           0
  Major Version Field                           2
  NVMe Errata Version				98
  PCIe Link Retraining Count			8084
  )EOF";

  mockFactory_->expectedCommands(
      {{{kNvmePath, "list", "-o", "json", "-v"}, kListOutput},
       {{kNvmePath, "ocp", "smart-add-log", "/dev/nvme0n1"},
        kSmartLogOutputOcp}});
  EXPECT_EQ(nandWriteBytes("nvme0n1", kNvmePath, mockFactory_), 88403820249088);
}

} // namespace hw
} // namespace facebook
