#ifndef SRZ80_LIFECYCLE_H
#define SRZ80_LIFECYCLE_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Optional ShouryoHost::query extension: "host.lifecycle.v1".
   The callback runs once on each transition from stopped or paused to running,
   before simulation time advances. Subscriptions belong to the card owner;
   host.cancel also cancels the returned handle. */
typedef struct SrhHostLifecycleV1 {
    SRH_HEADER;
    void *context;
    SrhStatus(SRH_CALL *subscribe_resume)(void *context, SrhHandle owner,
                                           SrhCallback callback, void *callback_context,
                                           SrhHandle *subscription);
} SrhHostLifecycleV1;

#ifdef __cplusplus
}
#endif
#endif
