// SPDX-License-Identifier: MIT
#pragma once

#include "Interface/Core/ArchHelpers/Arm64Emitter.h"

#include <FEXCore/Core/CPUBackend.h>
#include <FEXCore/fextl/memory.h>

#ifdef VIXL_SIMULATOR
#include <aarch64/simulator-aarch64.h>
#endif

#include <cstdint>
#include <signal.h>
#include <stddef.h>
#include <stack>
#include <tuple>

namespace FEXCore {
struct GuestSigAction;
}

namespace FEXCore::Core {
struct CpuStateFrame;
struct InternalThreadState;
}

namespace FEXCore::Context {
class ContextImpl;
}

namespace FEXCore::CPU {

struct DispatcherConfig {
  bool StaticRegisterAllocation = false;
};

#define STATE_PTR(STATE_TYPE, FIELD) \
  STATE.R(), offsetof(FEXCore::Core::STATE_TYPE, FIELD)

class Dispatcher final : public Arm64Emitter {
public:
  static fextl::unique_ptr<Dispatcher> Create(FEXCore::Context::ContextImpl *CTX, const DispatcherConfig &Config);

  Dispatcher(FEXCore::Context::ContextImpl *ctx, const DispatcherConfig &Config);
  ~Dispatcher() = default;

  /**
   * @name Dispatch Helper functions
   * @{ */
  uint64_t ThreadStopHandlerAddress{};
  uint64_t ThreadStopHandlerAddressSpillSRA{};
  uint64_t AbsoluteLoopTopAddress{};
  uint64_t AbsoluteLoopTopAddressFillSRA{};
  uint64_t ThreadPauseHandlerAddress{};
  uint64_t ThreadPauseHandlerAddressSpillSRA{};
  uint64_t ExitFunctionLinkerAddress{};
  uint64_t SignalHandlerReturnAddress{};
  uint64_t SignalHandlerReturnAddressRT{};
  uint64_t GuestSignal_SIGILL{};
  uint64_t GuestSignal_SIGTRAP{};
  uint64_t GuestSignal_SIGSEGV{};
  uint64_t IntCallbackReturnAddress{};

  uint64_t PauseReturnInstruction{};

  /**  @} */

  uint64_t Start{};
  uint64_t End{};

  void InitThreadPointers(FEXCore::Core::InternalThreadState *Thread);

  // These are across all arches for now
  static constexpr size_t MaxGDBPauseCheckSize = 128;

  size_t GenerateGDBPauseCheck(uint8_t *CodeBuffer, uint64_t GuestRIP);

#ifdef VIXL_SIMULATOR
  void ExecuteDispatch(FEXCore::Core::CpuStateFrame *Frame) ;
  void ExecuteJITCallback(FEXCore::Core::CpuStateFrame *Frame, uint64_t RIP);
#else
  void ExecuteDispatch(FEXCore::Core::CpuStateFrame *Frame) {
    DispatchPtr(Frame);
  }

  void ExecuteJITCallback(FEXCore::Core::CpuStateFrame *Frame, uint64_t RIP) {
    CallbackPtr(Frame, RIP);
  }
#endif

  uint16_t GetSRAGPRCount() const {
    return StaticRegisters.size();
  }

  uint16_t GetSRAFPRCount() const {
    return StaticFPRegisters.size();
  }

  void GetSRAGPRMapping(uint8_t Mapping[16]) const {
    for (size_t i = 0; i < StaticRegisters.size(); ++i) {
      Mapping[i] = StaticRegisters[i].Idx();
    }
  }

  void GetSRAFPRMapping(uint8_t Mapping[16]) const {
    for (size_t i = 0; i < StaticFPRegisters.size(); ++i) {
      Mapping[i] = StaticFPRegisters[i].Idx();
    }
  }

  const DispatcherConfig& GetConfig() const { return config; }

protected:
  FEXCore::Context::ContextImpl *CTX;
  DispatcherConfig config;

  static void SleepThread(FEXCore::Context::ContextImpl *ctx, FEXCore::Core::CpuStateFrame *Frame);

  static uint64_t GetCompileBlockPtr();

  using AsmDispatch = void(*)(FEXCore::Core::CpuStateFrame *Frame);
  using JITCallback = void(*)(FEXCore::Core::CpuStateFrame *Frame, uint64_t RIP);

  AsmDispatch DispatchPtr;
  JITCallback CallbackPtr;
private:
  // Long division helpers
  uint64_t LUDIVHandlerAddress{};
  uint64_t LDIVHandlerAddress{};
  uint64_t LUREMHandlerAddress{};
  uint64_t LREMHandlerAddress{};

  void EmitDispatcher();
};

}
