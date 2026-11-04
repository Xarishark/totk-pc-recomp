// The play-report service ("prepo:u") for Astral Chain 1.0.1.
//
// The game opens it during the start-up task, before any scene exists. A play report only feeds Nintendo's
// telemetry, so every command is accepted and returns no output: the SDK's client sees a successful request and the
// start-up task continues. The command id is logged so a command that turns out to need data can be implemented later.
#include <stdint.h>

#include "host.h"
#include "ipc.h"
#include "ov.h"

void prepo_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    (void)rs;
    static int logs;
    if (logs < 40) {
        rt_log("prepo:u: command %u data %u copy %u move %u", rq->cmd, rq->data_size, rq->ncopy, rq->nmove);
        logs++;
    }
}
