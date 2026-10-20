/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netease/playback_protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* POSIX replaces a destination atomically. Horizon's FS rename instead fails
 * with EEXIST. Keep a recoverable old queue throughout that fallback: IPC only
 * announces the new queue after publication has succeeded. */
static int replaceQueueFile(const char* temporary, const char* path, const char* backup) {
    if (rename(temporary, path) == 0) {
        remove(backup); /* A stale backup is no longer authoritative. */
        return 0;
    }
    if (errno != EEXIST) return -1;
    /* EEXIST established that path still contains the old queue. It is safe to
     * discard a backup from an earlier completed publication before saving it. */
    if (remove(backup) != 0 && errno != ENOENT) return -1;
    if (rename(path, backup) != 0) return -1;
    if (rename(temporary, path) == 0) {
        remove(backup);
        return 0;
    }
    const int publication_error = errno;
    /* If the SD card also rejects restoration, leave .bak intact. The engine
     * can load it when the primary queue is missing, including after a crash. */
    rename(backup, path);
    errno = publication_error;
    return -1;
}

int ncmWriteQueueFile(const char* path, const NcmQueueEntry* entries, uint32_t count) {
    if (!path || !entries || !count || count > NCM_MAX_QUEUE) { errno = EINVAL; return -1; }
    size_t len = strlen(path);
    char* temporary = malloc(len + 5);
    if (!temporary) return -1;
    char* backup = malloc(len + 5);
    if (!backup) { free(temporary); return -1; }
    memcpy(temporary, path, len);
    memcpy(temporary + len, ".tmp", 5);
    memcpy(backup, path, len);
    memcpy(backup + len, ".bak", 5);
    FILE* out = fopen(temporary, "wb");
    if (!out) { free(backup); free(temporary); return -1; }
    NcmQueueHeader header = { {'N','C','M','Q','U','E','U','E'}, NCM_PROTOCOL_VERSION, count };
    int good = fwrite(&header, sizeof(header), 1, out) == 1 &&
               fwrite(entries, sizeof(*entries), count, out) == count;
    int write_error = good ? 0 : (errno ? errno : EIO);
    if (fflush(out) != 0) good = 0;
    if (!good && !write_error) write_error = errno ? errno : EIO;
    if (fsync(fileno(out)) != 0) good = 0;
    if (!good && !write_error) write_error = errno ? errno : EIO;
    if (fclose(out) != 0) good = 0;
    if (!good && !write_error) write_error = errno ? errno : EIO;
    int result = good ? replaceQueueFile(temporary, path, backup) : -1;
    const int saved_error = good ? errno : write_error;
    if (result) remove(temporary);
    free(backup);
    free(temporary);
    if (result) errno = saved_error;
    return result;
}
