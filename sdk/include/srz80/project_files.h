#ifndef SRZ80_PROJECT_FILES_H
#define SRZ80_PROJECT_FILES_H
#include "abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional ShouryoHost::query extension: "host.project_files.v1".
   Check abi_version == SRH_ABI and struct_size >= sizeof(SrhHostProjectFilesV1).
   This is a convenience API for native cards, not a security boundary.
   Each call requires a live card owner. No active project directory returns
   SRH_UNAVAILABLE. Paths are UTF-8, relative to the project root, with either
   slash style. Absolute paths and traversal outside the root return SRH_INVALID.
   Missing files return SRH_NOT_FOUND; other filesystem failures return SRH_ERROR.
   Reads and writes are limited to 16 MiB and require regular files. Parents
   of written files must exist. Root project.json may be read but cannot be
   written through this API, regardless of case or dot-segment aliases. */
typedef struct SrhHostProjectFilesV1 {
    SRH_HEADER;
    void *context;
    /* UTF-8 absolute path, including trailing NUL. NULL buffer queries size.
       Too-small buffers return SRH_INVALID and update *size. */
    SrhStatus(SRH_CALL *project_root)(void *context, SrhHandle owner, char *buffer, uint64_t *size);
    /* Raw file bytes; NULL buffer queries size. Too-small buffers return
       SRH_INVALID and update *size. Zero-length files need no buffer. */
    SrhStatus(SRH_CALL *read_file)(void *context, SrhHandle owner, const char *relative_path,
                                   uint8_t *buffer, uint64_t *size);
    /* Atomic replacement in the target directory; size may be zero with NULL data. */
    SrhStatus(SRH_CALL *write_file)(void *context, SrhHandle owner, const char *relative_path,
                                    const uint8_t *data, uint64_t size);
} SrhHostProjectFilesV1;

#ifdef __cplusplus
}
#endif
#endif
