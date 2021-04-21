#include "Tests/LinuxSyscalls/x32/Types.h"
#include "Tests/LinuxSyscalls/x32/Ioctl/HelperDefines.h"

#include <cstdint>
#include <sound/asound.h>
#include <sys/ioctl.h>

namespace FEX::HLE::x32 {

namespace asound {
#include "Tests/LinuxSyscalls/x32/Ioctl/asound.inl"
}
}
