// The settings service ("set", settings:u) for Astral Chain 1.0.1.
//
// At boot the game's language setup calls ISettingsServer::MakeLanguageCode through the SDK's hipc2 client, and the
// SDK only reached "set" through sm:, so this service is the first thing the game needs that TotK never did (TotK's
// SDK took nn::oe::GetDesiredLanguage instead). The commands served are the ones nn::settings uses; every other
// command is a named fatal so the next missing one reports its id instead of failing silently.
//
// A LanguageCode is not a nn::settings::Language index: it is the code as an 8-byte string ("ja", "en-US", "fr", ...),
// the same form nn::oe::GetDesiredLanguage returns, and the game picks its language by comparing the two
// (nn::settings::operator== walks the bytes up to the NUL). nn::settings::LanguageCode::Make is answered by the host
// override below rather than by this service: the game builds 15 codes at boot to find the one that matches, and the
// hipc2 client of this SDK reads MakeLanguageCode's reply from its own buffer, which the CMIF reply does not fill (the
// game then compared empty codes and stayed on its first row, English). The service keeps its MakeLanguageCode reply
// for any caller that reads the message itself.
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "ov.h"

// The nn::settings::Language indexes (ja=0 .. pt-BR=17) and their LanguageCode string.
static const char *const language_codes[] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt",
    "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR",
};
#define N_LANGUAGE_CODES ((int32_t)(sizeof language_codes / sizeof *language_codes))

static uint64_t language_code(int32_t l) {
    if (l < 0 || l >= N_LANGUAGE_CODES) l = 0;   // outside the enum: ja, the code the real service starts at
    uint64_t v = 0;
    memcpy(&v, language_codes[l], strlen(language_codes[l]));
    return v;
}

// LanguageCode nn::settings::LanguageCode::Make(Language language): the SDK asks set: for it, which is a round trip
// through a service that cannot answer this client; the code is a pure function of the enum.
static void ov_language_code_make(Cpu *c) {
    ret_x(c, language_code((int32_t)arg_x(c, 0)));
}

void settings_server_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 2: {   // GetAvailableLanguageCodes2 -> s32 count (the codes buffer is the client's own; zeros there are valid ja)
        int32_t n = N_LANGUAGE_CODES;
        IPC_OUT(rs, n);
        return;
    }
    case 5: {   // MakeLanguageCode(s32 language) -> u64 LanguageCode: the code string of that language
        int32_t l = 0;
        memcpy(&l, ipc_arg(rq, 0, 4), 4);
        uint64_t v = language_code(l);
        IPC_OUT(rs, v);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

HOST_OVERRIDE("_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", ov_language_code_make);
