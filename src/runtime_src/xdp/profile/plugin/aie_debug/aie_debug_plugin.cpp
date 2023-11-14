/**
 * Copyright (C) 2022-2023 Advanced Micro Devices, Inc. - All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define XDP_SOURCE

#include "xdp/profile/plugin/aie_debug/aie_debug_plugin.h"

#include <boost/algorithm/string.hpp>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "core/common/api/hw_context_int.h"
#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "core/common/system.h"
#include "core/common/xrt_profiling.h"
#include "core/include/experimental/xrt-next.h"

#include "op_types.h"
#include "op_buf.hpp"
#include "op_init.hpp"

#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/database.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/device/device_intf.h"
#include "xdp/profile/device/hal_device/xdp_hal_device.h"
#include "xdp/profile/device/utility.h"
#include "xdp/profile/plugin/vp_base/info.h"

constexpr std::uint64_t CONFIGURE_OPCODE = std::uint64_t{2};

namespace xdp {
  using severity_level = xrt_core::message::severity_level;
  namespace pt = boost::property_tree;

  bool AieDebugPlugin::live = false;

  AieDebugPlugin::AieDebugPlugin() : XDPPlugin()
  {
    xrt_core::message::send(severity_level::info, "XRT", "Instantiating AIE Debug Plugin.");
    AieDebugPlugin::live = true;

    db->registerPlugin(this);
    db->getStaticInfo().setAieApplication();
  }

  AieDebugPlugin::~AieDebugPlugin()
  {
    xrt_core::message::send(severity_level::info, "XRT", "Destroying AIE Debug Plugin.");
    // Stop the polling thread
    endPoll();

    if (VPDatabase::alive()) {
      db->unregisterPlugin(this);
    }

    AieDebugPlugin::live = false;
  }

  bool AieDebugPlugin::alive()
  {
    return AieDebugPlugin::live;
  }

  uint64_t AieDebugPlugin::getDeviceIDFromHandle(void* handle)
  {
    auto itr = handleToAIEData.find(handle);
    if (itr != handleToAIEData.end())
      return itr->second.deviceID;

#ifdef XDP_MINIMAL_BUILD
    return db->addDevice("win_device");
#else
    constexpr uint32_t PATH_LENGTH = 512;
    
    char pathBuf[PATH_LENGTH];
    memset(pathBuf, 0, PATH_LENGTH);

    xclGetDebugIPlayoutPath(handle, pathBuf, PATH_LENGTH);
    std::string sysfspath(pathBuf);
    return db->addDevice(sysfspath);  // Get the unique device Id
#endif
  }

  void AieDebugPlugin::updateAIEDevice(void* handle) {
    xrt_core::message::send(severity_level::info, "XRT", "Calling AIE Debug Update Device.");

    if (!xrt_core::config::get_aie_debug())
      return;

  
    try {
      pt::read_json("aie_control_config.json", aie_meta);
      filetype = aie::readAIEMetadata("aie_control_config.json", aie_meta);
    } catch (...) {
      std::stringstream msg;
      msg << "The file aie_control_config.json is required in the same directory as the host executable to run AIE Profile.";
      xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      return;
    }

    context = xrt_core::hw_context_int::create_hw_context_from_implementation(handle);
    
    xdp::aie::driver_config meta_config = getAIEConfigMetadata();

    XAie_Config cfg {
      meta_config.hw_gen,
      meta_config.base_address,
      meta_config.column_shift,
      meta_config.row_shift,
      meta_config.num_rows,
      meta_config.num_columns,
      meta_config.shim_row,
      meta_config.mem_row_start,
      meta_config.mem_num_rows,
      meta_config.aie_tile_row_start,
      meta_config.aie_tile_num_rows,
      {0} // PartProp
    };

    auto regValues = parseMetrics();
  
    const module_type moduleTypes[NUM_MODULES] =
      {module_type::core, module_type::dma, module_type::shim, module_type::mem_tile};
    std::vector<profile_data_t> op_profile_data;

    int counterId = 0;
    for (int module = 0; module < NUM_MODULES; ++module) {
      auto type = moduleTypes[module];
    
      std::vector<tile_type> tiles;
      if (type == module_type::shim) {
        tiles = filetype->getInterfaceTiles("all", "all", "", -1);
      } else {
        tiles = filetype->getTiles("all", type, "all");
      }

      if (tiles.empty()) {
        std::stringstream msg;
        msg << "AIE Debug found no tiles for module: " << module << ".";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }
      
      std::vector<uint64_t> Regs = regValues[type];

      for (auto &tile : tiles) {
        for (int i = 0; i < Regs.size(); i++){
          std::stringstream msg;
          msg << "AIE Debug monitoring AIE tile (" << tile.col << "," 
            << tile.row << ") in module " << module << ".";
          xrt_core::message::send(severity_level::debug, "XRT", msg.str());
          op_profile_data.emplace_back(profile_data_t{Regs[i] + (tile.col << 25) + (tile.row << 20)});
          counterId++;
        }
      }
    }

    auto RC = XAie_CfgInitialize(&aieDevInst, &cfg);
    if (RC != XAIE_OK) {
      xrt_core::message::send(severity_level::warning, "XRT", "AIE Driver Initialization Failed.");
      return;
    }

    op_size = sizeof(aie_profile_op_t) + sizeof(profile_data_t) * (counterId - 1);
    op = (aie_profile_op_t*)malloc(op_size);
    op->count = counterId;
    for (int i = 0; i < op_profile_data.size(); i++) {
      op->profile_data[i] = op_profile_data[i];
    }
  }


  void AieDebugPlugin::endAIEDebugRead(void* handle)
  {
    (void)handle;
    endPoll();
  }

  std::map<module_type, std::vector<uint64_t>> AieDebugPlugin::parseMetrics() {

    std::map<module_type, std::vector<uint64_t>> regValues {
          {module_type::core, {}}, 
          {module_type::dma, {}}, 
          {module_type::shim, {}}, 
          {module_type::mem_tile, {}}, 
        };

    std::vector<std::string> metricsConfig;

    const module_type moduleTypes[NUM_MODULES] =
      {module_type::core, module_type::dma, module_type::shim, module_type::mem_tile};

    metricsConfig.push_back(xrt_core::config::get_aie_debug_settings_core_registers());
    metricsConfig.push_back(xrt_core::config::get_aie_debug_settings_memory_registers());
    metricsConfig.push_back(xrt_core::config::get_aie_debug_settings_interface_registers());
    metricsConfig.push_back(xrt_core::config::get_aie_debug_settings_memory_tile_registers());

    for (int module = 0; module < NUM_MODULES; ++module) {
      auto type = moduleTypes[module];
      std::vector<std::string> metricsSettings = getSettingsVector(metricsConfig[module]);

      for (auto& s : metricsSettings) {
        try {
          uint64_t val = stoul(s,nullptr,16);
          regValues[type].push_back(val);
        } catch (...) {
            xrt_core::message::send(severity_level::warning, "XRT", "Error Parsing Metric String.");
        }
      }
    }

    return regValues;
  
  }

  std::vector<std::string> AieDebugPlugin::getSettingsVector(std::string settingsString)
  {
    if (settingsString.empty())
      return {};

    // Each of the metrics can have ; separated multiple values. Process and save all
    std::vector<std::string> settingsVector;
    boost::replace_all(settingsString, " ", "");
    boost::split(settingsVector, settingsString, boost::is_any_of(","));
    return settingsVector;
  }

  void AieDebugPlugin::endPoll()
  {
    xrt_core::message::send(severity_level::info, "XRT", "Calling AIE Debug endPoll.");
    XAie_StartTransaction(&aieDevInst, XAIE_TRANSACTION_DISABLE_AUTO_FLUSH);
    // Profiling is 3rd custom OP
    XAie_RequestCustomTxnOp(&aieDevInst);
    XAie_RequestCustomTxnOp(&aieDevInst);
    auto read_op_code_ = XAie_RequestCustomTxnOp(&aieDevInst);

    try {
      mKernel = xrt::kernel(context, "XDP_KERNEL");  
    } catch (std::exception &e){
      std::stringstream msg;
      msg << "Unable to find XDP_KERNEL kernel from hardware context. Not configuring AIE Debug. " << e.what() ;
      xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      return;
    }

    XAie_AddCustomTxnOp(&aieDevInst, (uint8_t)read_op_code_, (void*)op, op_size);
    uint8_t *txn_ptr = XAie_ExportSerializedTransaction(&aieDevInst, 1, 0);
    op_buf instr_buf;
    instr_buf.addOP(transaction_op(txn_ptr));

    // this BO stores polling data and custom instructions
    xrt::bo instr_bo;
    try {
      instr_bo = xrt::bo(context.get_device(), instr_buf.ibuf_.size(), XCL_BO_FLAGS_CACHEABLE, mKernel.group_id(1));
    } catch (std::exception &e){
      std::stringstream msg;
      msg << "Unable to create the instruction buffer for polling during AIE Debug. " << e.what() << std::endl;
      xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      return;
    }

    instr_bo.write(instr_buf.ibuf_.data());
    instr_bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    auto run = mKernel(CONFIGURE_OPCODE, instr_bo, instr_bo.size()/sizeof(int), 0, 0, 0, 0);
    try {
      run.wait2();
    } catch (std::exception &e) {
      std::stringstream msg;
      msg << "Unable to successfully execute AIE Profile polling kernel. " << e.what() << std::endl;
      xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      return;
    }

    XAie_ClearTransaction(&aieDevInst);

    static constexpr uint32_t size_4K   = 0x1000;
    static constexpr uint32_t offset_3K = 0x0C00;

    // results BO syncs AIE Debug result from device
    xrt::bo result_bo;
    try {
      result_bo = xrt::bo(context.get_device(), size_4K, XCL_BO_FLAGS_CACHEABLE, mKernel.group_id(1));
    } catch (std::exception &e) {
      std::stringstream msg;
      msg << "Unable to create result buffer for AIE Debug. Cannot get AIE Debug Info." << e.what() << std::endl;
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg.str());
      return;
    }

    auto result_bo_map = result_bo.map<uint8_t*>();
    result_bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

    uint32_t* output = reinterpret_cast<uint32_t*>(result_bo_map+offset_3K);

    for (uint32_t i = 0; i < op->count; i++) {
      std::stringstream msg;
      int col = (op->profile_data[i].perf_address >> 25) & 0x1F;
      int row = (op->profile_data[i].perf_address >> 20) & 0x1F;
      int reg = (op->profile_data[i].perf_address) & 0xFFFFF;
      
      msg << "Debug tile (" << col << ", " << row << ") " << "address/values: 0x" << std::hex << reg << ": " << std::dec << output[i];
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", msg.str());
    }

    free(op);
  }



  aie::driver_config
  AieDebugPlugin::getAIEConfigMetadata()
  {
    return filetype->getDriverConfig();
  }

}  // end namespace xdp

