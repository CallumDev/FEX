#include "Interface/Context/Context.h"

#include "Interface/Core/ArchHelpers/Arm64.h"
#include "Interface/Core/ArchHelpers/MContext.h"
#include "Interface/Core/Dispatcher/Dispatcher.h"
#include "Interface/Core/Interpreter/InterpreterClass.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/LogManager.h>

#include <memory>
#include <signal.h>
#include <stdint.h>
#include <unordered_map>
#include <utility>

#include "InterpreterOps.h"

#if defined(_M_X86_64)
  #include "Interface/Core/Dispatcher/X86Dispatcher.h"
#elif defined(_M_ARM_64)
  #include "Interface/Core/Dispatcher/Arm64Dispatcher.h"
#else
  #error missing arch
#endif

namespace FEXCore::IR {
  class IRListView;
  class RegisterAllocationData;
}

namespace FEXCore::CPU {
class CPUBackend;

InterpreterCore::InterpreterCore(FEXCore::Context::Context *ctx, FEXCore::Core::InternalThreadState *Thread)
  : CPUBackend(Thread, 1024 * 1024 * 16, 1024 * 1024 * 128)
  , CTX {ctx} {

  DispatcherConfig config;
  config.InterpreterDispatch = true;

#if defined(_M_X86_64)
  Dispatcher = std::make_unique<X86Dispatcher>(ctx, Thread, config);
#elif defined(_M_ARM_64)
  Dispatcher = std::make_unique<Arm64Dispatcher>(ctx, Thread, config);
#else
  #error missing arch
#endif

  DispatchPtr = Dispatcher->DispatchPtr;
  CallbackPtr = Dispatcher->CallbackPtr;

  auto &Interpreter = Thread->CurrentFrame->Pointers.Interpreter;

  Interpreter.FragmentExecuter = reinterpret_cast<uint64_t>(&InterpreterOps::InterpretIR); 
  Interpreter.CallbackReturn = Dispatcher->ReturnPtr;
}

void InterpreterCore::InitializeSignalHandlers(FEXCore::Context::Context *CTX) {
  CTX->SignalDelegation->RegisterHostSignalHandler(SignalDelegator::SIGNAL_FOR_PAUSE, [](FEXCore::Core::InternalThreadState *Thread, int Signal, void *info, void *ucontext) -> bool {
    InterpreterCore *Core = reinterpret_cast<InterpreterCore*>(Thread->CPUBackend.get());
    return Core->Dispatcher->HandleSignalPause(Signal, info, ucontext);
  }, true);

#ifdef _M_ARM_64
  CTX->SignalDelegation->RegisterHostSignalHandler(SIGBUS, [](FEXCore::Core::InternalThreadState *Thread, int Signal, void *info, void *ucontext) -> bool {
    return FEXCore::ArchHelpers::Arm64::HandleSIGBUS(true, Signal, info, ucontext);
  }, true);
#endif

  auto GuestSignalHandler = [](FEXCore::Core::InternalThreadState *Thread, int Signal, void *info, void *ucontext, GuestSigAction *GuestAction, stack_t *GuestStack) -> bool {
    InterpreterCore *Core = reinterpret_cast<InterpreterCore*>(Thread->CPUBackend.get());
    return Core->Dispatcher->HandleGuestSignal(Signal, info, ucontext, GuestAction, GuestStack);
  };

  for (uint32_t Signal = 0; Signal <= SignalDelegator::MAX_SIGNALS; ++Signal) {
    CTX->SignalDelegation->RegisterHostSignalHandlerForGuest(Signal, GuestSignalHandler);
  }
}

void *InterpreterCore::CompileCode(uint64_t Entry, [[maybe_unused]] FEXCore::IR::IRListView const *IR, [[maybe_unused]] FEXCore::Core::DebugData *DebugData, FEXCore::IR::RegisterAllocationData *RAData) {

  auto dst = nullptr;

  // check available size for 
  auto sz = IR->GetInlineSize();
  // serialize
  IR->Serialize(dst);
  return dst;
}

std::unique_ptr<CPUBackend> CreateInterpreterCore(FEXCore::Context::Context *ctx, FEXCore::Core::InternalThreadState *Thread) {
  return std::make_unique<InterpreterCore>(ctx, Thread);
}

void InitializeInterpreterSignalHandlers(FEXCore::Context::Context *CTX) {
  InterpreterCore::InitializeSignalHandlers(CTX);
}

}
