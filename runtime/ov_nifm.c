// The network interface manager service ("nifm:u") for Astral Chain 1.0.1.
//
// The game opens nifm:u while it starts up, before the intro menu. Nothing on this host needs a network, so every
// command is accepted and returns no output: the SDK's client sees a successful request with a zero-filled reply buffer,
// which is the "no network" answer the game already has to handle. The command id is logged so the first command that
// actually needs data can be implemented with its real layout.
#include <stdint.h>

#include "host.h"
#include "ipc.h"
#include "ov.h"

void nifm_user_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    (void)rs;
    rt_log("nifm:u: command %u", rq->cmd);
}

// The game imports nn::nifm::Initialize and calls it from its start-up task; the SDK's implementation opens nifm:u
// and fails on the missing CMIF domain support (0xE401), which makes the start-up task exit before a scene exists.
// The port has no network, so accept the call without opening the service.
static void ov_nifm_initialize(Cpu *c) { c->x[0] = 0; }
HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ov_nifm_initialize);
