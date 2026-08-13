#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../kernel/core/syscall_ipc.hpp"
#include "../../kernel/core/ipc.hpp"

using namespace auroraos;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < sizeof(kernel::IpcRawMessage) || size > 256)
        return 0;

    // Create a dummy message
    char msg[256];
    memcpy(msg, data, size);

    // We cannot call sys_ipc_call directly because it requires a valid TaskControlBlock and CSpace.
    // However, we can fuzz the raw message parser if any.
    // Since IPC in AuroraOS is just passing raw bytes, we can fuzz higher-level protocols.
    // E.g., the firewall IPC command protocol

    return 0;
}
