#ifndef SRZ80_SIGNALS_H
#define SRZ80_SIGNALS_H
#include <srz80/abi.h>
#ifdef __cplusplus
extern "C" {
#endif
/* host.signals.v1: release an owner's driver, exposing other drivers or idle. */
typedef struct SrhHostSignalsV1 {
    SRH_HEADER;
    void *context;
    SrhStatus(SRH_CALL *release)(void *, SrhHandle owner, SrhHandle signal);
} SrhHostSignalsV1;
#ifdef __cplusplus
}
#endif
#endif
