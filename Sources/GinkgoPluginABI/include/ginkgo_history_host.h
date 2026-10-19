#ifndef GINKGO_HISTORY_HOST_H
#define GINKGO_HISTORY_HOST_H

/*
 * Ginkgo history-append host ABI, version 1. Companion to ginkgo_plugin.h:
 * an optional side vtable so a plugin granted `historyAppend` can ask the
 * host to record a clip without writing the pasteboard and without growing
 * ginkgo_plugin_functions (GINKGO_PLUGIN_INTERFACE_VERSION stays 1).
 *
 * How the two sides meet:
 *   - The plugin bundle is a regular .ginkgoplugin. When its manifest
 *     declares the historyAppend capability it MUST also export
 *     GINKGO_PLUGIN_BIND_HOST; the host rejects the package at load if the
 *     symbol is missing (fail closed). A plugin that exports the symbol
 *     without declaring the capability is never bound.
 *   - After the user grants historyAppend, the host builds a
 *     ginkgo_history_host_v1 in memory and calls the bind entry once with
 *     a pointer to that table. The plugin stores the pointer and may call
 *     create_clip later.
 *   - create_clip is only available during convert. The host refuses the
 *     call (error "unavailable") during match, outside either phase, or
 *     when a create_clip is already on the stack. A missing grant returns
 *     "notGranted".
 *
 * String rules (mirroring ginkgo_plugin.h):
 *   - A char* create_clip RETURNS is host-owned until the plugin passes it
 *     back through free_string; copy before freeing, never use after.
 *   - request_json is caller-owned and valid for the duration of the call.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GINKGO_HISTORY_HOST_VERSION 1

#define GINKGO_PLUGIN_BIND_HOST "ginkgo_plugin_bind_host_v1"

#define GINKGO_CAPABILITY_HISTORY_APPEND "historyAppend"

typedef struct ginkgo_history_host_v1 {
    uint32_t interface_version; /* GINKGO_HISTORY_HOST_VERSION */
    void *context;
    /*
     * Caller-owned request JSON, valid for the call.
     * Success: {"id":"<uri>","created":true|false}
     * Refusal: {"error":"empty"|"insertLimit"|"saveFailed"|"malformed"|"unavailable"|"notGranted"}
     * Host-owned heap string. Plugin copies, then free_string.
     */
    char *(*create_clip)(void *context, const char *request_json);
    void (*free_string)(void *context, char *string);
} ginkgo_history_host_v1;

typedef void (*ginkgo_plugin_bind_host_t)(const ginkgo_history_host_v1 *host);

#ifdef __cplusplus
}
#endif

#endif /* GINKGO_HISTORY_HOST_H */
