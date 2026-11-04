// The filesystem proxy service ("fsp-srv") for Astral Chain 1.0.1.
//
// The port serves the file systems themselves in runtime/fs_host.c and overrides the nn::fs entry points it can reach.
// The SDK still opens fsp-srv for one side path: its access-log switch (GetGlobalAccessLogMode). That switch must answer
// success with mode 0 (an error makes the SDK abort). The save data, including the commit path's own IFileSystem object,
// never reaches this port: runtime/fs_host.c overrides MountSaveData, EnsureSaveData, Commit and CommitSaveData.
#include <stdint.h>

#include "host.h"
#include "ipc.h"
#include "ov.h"

void fsp_srv_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    static int logs;
    if (logs < 40) {
        rt_log("fsp-srv: command %u data %u copy %u move %u", rq->cmd, rq->data_size, rq->ncopy, rq->nmove);
        logs++;
    }
    switch (rq->cmd) {
    case 1004:   // SetGlobalAccessLogMode
    case 1006:   // OutputAccessLogToSdCard
    case 1016:   // FlushAccessLogOnSdCard
        return;
    case 1005: {   // GetGlobalAccessLogMode: logging off, so IsEnabledAccessLog returns false
        uint32_t mode = 0;
        IPC_OUT(rs, mode);
        return;
    }
    case 1011: {   // GetProgramIndexForAccessLog: version and program index
        uint32_t v[2] = {2, 0};
        IPC_OUT(rs, v);
        return;
    }
    default:
        // The remaining commands are reached only by the SDK's own mount bookkeeping. The host fs layer has already
        // answered the game's mounts, so accept them without an object instead of stopping the boot.
        return;
    }
}
