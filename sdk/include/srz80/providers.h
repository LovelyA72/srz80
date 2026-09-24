#ifndef SRZ80_PROVIDERS_H
#define SRZ80_PROVIDERS_H
#include <srz80/abi.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional opaque card-data transport. Core does not interpret data or
   revisions. Providers own their payload schema, identified by protocol.
   All callbacks run on the simulation thread. Strings are UTF-8; all copied
   buffers use a size query / caller-owned copy, including the terminating NUL. */
#define SRH_PROVIDER_MAX_BYTES (1024u * 1024u)
#define SRH_PROVIDER_LIVE_CONFIG 1u
enum { SRH_PROVIDER_INTERACTION = 0, SRH_PROVIDER_CONFIGURE = 1 };
typedef struct SrhDataProviderV1 {
    SRH_HEADER;
    void *context;
    SrhStatus(SRH_CALL *snapshot)(void *, char *, uint64_t *);
    SrhStatus(SRH_CALL *command)(void *, uint32_t, uint64_t, const char *, uint64_t);
    /* Required metadata is copied at registration. No inferred names/types. */
    char name[128];
    char protocol[128];
    /* Optional tail. Configuration commands are stopped-only unless enabled. */
    uint32_t flags;
} SrhDataProviderV1;
typedef struct SrhHostProvidersV1 {
    SRH_HEADER;
    void *context;
    SrhStatus(SRH_CALL *register_provider)(void *, SrhHandle, const SrhDataProviderV1 *);
    SrhStatus(SRH_CALL *simulation_time_ns)(void *, uint64_t *);
} SrhHostProvidersV1;
#ifdef __cplusplus
}
#endif
#endif
