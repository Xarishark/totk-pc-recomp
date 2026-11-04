// The hid service (IHidServer) for Astral Chain 1.0.1.
//
// The game calls nn::hid functions two ways: the ones main links directly are replaced by the HOST_OVERRIDE entries in
// ov_input.c, but the SDK's own hid client (hipc2, SDK 6.4.0) talks to the "hid" service for things like
// SetNpadHandheldActivationMode, so the service must exist. Commands are added as the boot run reports them; anything
// else is a named fatal that prints its id and argument size.
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "ov.h"

void hid_server_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 101: {   // GetSupportedNpadStyleSet(aruid...) -> u32 BitFlagSet<NpadStyleTag>
        uint32_t styles = 1u;   // FullKey, matching the host's one full-key controller
        IPC_OUT(rs, styles);
        return;
    }
    case 121: {   // GetNpadJoyHoldType(aruid...) -> u32 NpadJoyHoldType
        uint32_t hold = 0u;   // Vertical, the only sensible hold for a full-key pad
        IPC_OUT(rs, hold);
        return;
    }
    case 128: {   // SetNpadHandheldActivationMode(NpadHandheldActivationMode mode, aruid)
        uint32_t mode = 0;
        memcpy(&mode, ipc_arg(rq, 0, 4), 4);
        rt_log("hid: SetNpadHandheldActivationMode(%u)", mode);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
