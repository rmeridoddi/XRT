/**
 * Copyright (C) 2019-2022 Xilinx, Inc
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

#define XDP_PLUGIN_SOURCE

#include <iostream>

#include "core/common/message.h"
#include "core/include/xrt/xrt_kernel.h"
#include "xdp/profile/database/database.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/device/aie_trace/aie_trace_logger.h"
#include "xdp/profile/device/aie_trace/client/aie_trace_offload_client.h"
#include "xdp/profile/device/device_intf.h"

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  AIETraceOffload::AIETraceOffload(void* handle, uint64_t id, DeviceIntf* dInt,
                                   AIETraceLogger* logger, bool isPlio,
                                   uint64_t totalSize, uint64_t numStrm,
                                   xrt::hw_context context,
                                   std::shared_ptr<AieTraceMetadata>(metadata))
    : deviceHandle(handle), deviceId(id), deviceIntf(dInt), traceLogger(logger),
      isPLIO(isPlio), totalSz(totalSize), numStream(numStrm),
      traceContinuous(false), offloadIntervalUs(0), bufferInitialized(false),
      offloadStatus(AIEOffloadThreadStatus::IDLE), mEnCircularBuf(false),
      mCircularBufOverwrite(false), context(context), metadata(metadata)
  {
    bufAllocSz = deviceIntf->getAlignedTraceBufSize(
                   totalSz, static_cast<unsigned int>(numStream));
    mReadTrace =
      std::bind(&AIETraceOffload::readTraceGMIO, this, std::placeholders::_1);
  }

  bool AIETraceOffload::initReadTrace()
  {
    buffers.clear();
    buffers.resize(numStream);

    constexpr std::uint64_t DDR_AIE_ADDR_OFFSET = std::uint64_t{0x80000000};

    transactionHandler = std::make_unique<aie::ClientTransaction>(context, "AIE Trace Offload");

    if (!transactionHandler->initializeKernel("XDP_KERNEL"))
      return false;

    xdp::aie::driver_config meta_config = metadata->getAIEConfigMetadata();

    XAie_Config cfg{
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

    auto RC = XAie_CfgInitialize(&aieDevInst, &cfg);

    if (RC != XAIE_OK) {
      xrt_core::message::send(severity_level::warning, "XRT",
                              "AIE Driver Initialization Failed.");
      return false;
    }

    for (uint64_t i = 0; i < numStream; ++i) {
      VPDatabase* db = VPDatabase::Instance();
      TraceGMIO* traceGMIO = (db->getStaticInfo()).getTraceGMIO(deviceId, i);

      std::string tracemsg = "Allocating trace buffer of size " +
                             std::to_string(bufAllocSz) + " for AIE Stream " +
                             std::to_string(i);
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
                              tracemsg.c_str());
      xrt_bos.emplace_back(xrt::bo(context.get_device(), bufAllocSz,
                                   XRT_BO_FLAGS_HOST_ONLY, transactionHandler->getGroupID(0)));
      
      if (!xrt_bos.empty()) {
        auto bo_map = xrt_bos.back().map<uint8_t*>();
        memset(bo_map, 0, bufAllocSz);
      }
      // Start recording the transaction
      XAie_StartTransaction(&aieDevInst, XAIE_TRANSACTION_DISABLE_AUTO_FLUSH);

      // AieRC RC;
      // Todo: get this from aie metadata
      XAie_LocType loc;
      XAie_DmaDesc DmaDesc;
      loc = XAie_TileLoc(static_cast<uint8_t>(traceGMIO->shimColumn), 0);
      uint8_t s2mm_ch_id = static_cast<uint8_t>(traceGMIO->channelNumber);
      uint8_t s2mm_bd_id = 15; /* for now use last bd */

      // S2MM BD
      RC = XAie_DmaDescInit(&aieDevInst, &DmaDesc, loc);
      RC =
        XAie_DmaSetAddrLen(&DmaDesc, xrt_bos[i].address() + DDR_AIE_ADDR_OFFSET,
                           static_cast<uint32_t>(bufAllocSz));
      RC = XAie_DmaEnableBd(&DmaDesc);
      RC = XAie_DmaSetAxi(&DmaDesc, 0U, 8U, 0U, 0U, 0U);
      RC = XAie_DmaWriteBd(&aieDevInst, &DmaDesc, loc, s2mm_bd_id);

      // printf("Enabling channels....\n");
      RC = XAie_DmaChannelPushBdToQueue(&aieDevInst, loc, s2mm_ch_id, DMA_S2MM,
                                        s2mm_bd_id);
      RC = XAie_DmaChannelEnable(&aieDevInst, loc, s2mm_ch_id, DMA_S2MM);

      uint8_t* txn_ptr = XAie_ExportSerializedTransaction(&aieDevInst, 1, 0);
   
      if (!transactionHandler->submitTransaction(txn_ptr))
        return false;
      
      xrt_core::message::send(
        severity_level::info, "XRT",
        "Successfully scheduled AIE Trace Offloading Transaction Buffer.");

      // Must clear aie state
      XAie_ClearTransaction(&aieDevInst);
    }

    bufferInitialized = true;
    return bufferInitialized;
  }

  void AIETraceOffload::readTraceGMIO(bool /*final*/)
  {
    for (int index = 0; index < numStream; index++) {
      syncAndLog(index);
    }
  }

  uint64_t AIETraceOffload::syncAndLog(uint64_t index)
  {
    xrt_bos[index].sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    auto in_bo_map = xrt_bos[index].map<uint32_t*>();

    if (!in_bo_map)
      return 0;

    uint64_t nBytes = searchWrittenBytes((void*)in_bo_map, bufAllocSz);

    // Log nBytes of trace
    traceLogger->addAIETraceData(index, (void*)in_bo_map, nBytes, true);
    return xrt_bos[index].size();
  }

  /*
   * Implement these later for continuous offload
   */

  AIETraceOffload::~AIETraceOffload() {}

  void AIETraceOffload::startOffload() {}

  bool AIETraceOffload::keepOffloading() { return false; }

  void AIETraceOffload::stopOffload() {}

  void AIETraceOffload::offloadFinished() {}

  void AIETraceOffload::endReadTrace() {}

  uint64_t AIETraceOffload::searchWrittenBytes(void* buf, uint64_t bytes)
  {
    /*
     * Look For trace boundary using binary search.
     * Use Dword to be safe
     */
    auto words = static_cast<uint64_t*>(buf);
    uint64_t wordcount = bytes / TRACE_PACKET_SIZE;

    // indices
    int64_t low = 0;
    int64_t high = static_cast<int64_t>(wordcount) - 1;

    // Boundary at which trace ends and 0s begin
    uint64_t boundary = wordcount;

    while (low <= high) {
      int64_t mid = low + (high - low) / 2;

      if (!words[mid]) {
        boundary = mid;
        high = mid - 1;
      }
      else {
        low = mid + 1;
      }
    }

    uint64_t written = boundary * TRACE_PACKET_SIZE;

    debug_stream << "Found Boundary at 0x" << std::hex << written << std::dec
                 << std::endl;

    return written;
  }

} // namespace xdp
