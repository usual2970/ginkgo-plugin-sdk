#ifndef GINKGO_SYNC_ABI_H
#define GINKGO_SYNC_ABI_H

/*
 * Ginkgo encrypted history sync ABI, version 1 (encrypted-history-sync plan
 * U4). The companion to ginkgo_plugin.h for the OFFICIAL sync plugin
 * (dev.ginkgo.sync): the plugin owns cryptography and the sync-protocol
 * client; the host owns history storage, scheduling, and Settings UX. This
 * header freezes the seam between them as plain C.
 *
 * How the two sides meet:
 *   - The plugin bundle is a regular .ginkgoplugin (ginkgo_plugin.h) whose
 *     match/convert are unused (runsOnCopy false). It ADDITIONALLY exports
 *     GINKGO_SYNC_CLIENT_ENTRY, which the host resolves with dlsym — but
 *     ONLY for the allowlisted official id (the host never wires the table
 *     below into any other bundle's code).
 *   - The host builds a ginkgo_sync_host_v1 in memory and hands a pointer
 *     to it as the sole argument of run_sync_cycle. Every host callback
 *     takes the table's own `context` pointer back as its first argument.
 *
 * String rules (mirroring ginkgo_plugin.h):
 *   - A char* a table RETURNED stays owned by that table's side until the
 *     other side passes it back through that table's free_string; copy
 *     before freeing, never use after.
 *   - A const char* parameter is caller-owned and valid for the duration
 *     of the call only.
 *
 * Threading:
 *   - The host calls every client function from a single serialized
 *     background queue, never the main thread.
 *   - The plugin may invoke host callbacks from whichever thread it runs
 *     the cycle on; the host hops to its own serialization internally.
 *
 * JSON wire format: authoritative in plugin-sdk/docs/sync-protocol.md
 * (syncSchemaVersion 1). Records are opaque to the SERVER (the plugin
 * encrypts them); the shapes below are the DECRYPTED host<->plugin spelling.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The interface version this header describes. A table whose
 * interface_version differs is rejected before any call: bump only for a
 * breaking change, and never re-use a version number. */
#define GINKGO_SYNC_ABI_VERSION 1

/* The additional symbol the official sync plugin exports (dlsym'd by the
 * host after the bundle passed the standard external-plugin validation). */
#define GINKGO_SYNC_CLIENT_ENTRY ginkgo_sync_client_entry_v1

/* ------------------------------------------------------------------ */
/* Host side: callbacks the host implements.                           */
/* ------------------------------------------------------------------ */

/*
 * export_changes_since returns (heap JSON string, host-owned):
 *   {"records":   [ <sync record DTO>, ... ],   // tombstones included
 *    "blobs":     { "<blobID>": "<base64 plaintext>", ... },
 *    "nextRevision": <int>}                     // cursor to persist
 * or NULL on failure. Records carry blob REFERENCES; the bytes ride in
 * "blobs" so the plugin can encrypt and upload each once, content-addressed.
 *
 * apply_changes takes
 *   records_json: {"records": [ <sync record DTO>, ... ]}  (no bytes)
 *   blobs_json:   { "<blobID>": "<base64 plaintext>", ... }
 * and returns {"applied":N,"stale":N,"unknownTombstones":N} or NULL.
 * Idempotent under LWW; applying a batch twice is always safe.
 *
 * read_blob_for_record returns
 *   {"data": "<base64>" | null, "displayName": "..."}   // null = file
 *                                                      // unavailable
 * or NULL when the record / index is unknown. file_index < 0 addresses
 * the record's image bytes.
 *
 * notify_status receives the plugin's progress/state JSON (documented in
 * sync-protocol.md); the host surfaces it in Settings.
 */
typedef struct ginkgo_sync_host_v1 {
    /* Must equal GINKGO_SYNC_ABI_VERSION; checked before use. */
    uint32_t interface_version;

    /* Opaque host state; passed back as the first argument of every
     * callback. The plugin never interprets or stores it. */
    void *context;

    char *(*export_changes_since)(void *context, int64_t since_revision, int32_t limit);
    char *(*apply_changes)(void *context, const char *records_json, const char *blobs_json);
    char *(*read_blob_for_record)(void *context, const char *sync_record_id, int32_t file_index);
    void (*notify_status)(void *context, const char *status_json);
    void (*free_string)(void *context, char *string);
} ginkgo_sync_host_v1;

/* ------------------------------------------------------------------ */
/* Plugin side: the table the official sync plugin exports.            */
/* ------------------------------------------------------------------ */

/*
 * get_state returns (heap JSON string, plugin-owned):
 *   {"configured":   <bool>,
 *    "vaultId":      "<uuid>",        // present once configured
 *    "deviceId":     "<uuid>",
 *    "deviceName":   "...",
 *    "lastSyncAt":   <unix seconds>,  // present after a first pass
 *    "lastError":    "...",           // present when the last pass failed
 *    "quota": {"usedBytes":N,"limitBytes":N,"remainingBytes":N}}
 *
 * create_vault / join_vault take
 *   {"passphrase":"...","deviceName":"..."}            // create
 *   {"code":"...","passphrase":"...","deviceName":"..."}  // join
 * and return the get_state shape on success, or
 *   {"error":"<machine-readable>","message":"<human>"}  on failure.
 * Keys, tokens, and the export cursor persist in the plugin's own state
 * (never the pasteboard, never the host's history store).
 *
 * run_sync_cycle performs ONE full pass — push local deltas and blobs,
 * pull and apply remote changes, then notify_status — returning 0 on
 * success and non-zero on failure. It must tolerate being called with no
 * vault configured (a no-op success).
 *
 * cancel asks the in-flight cycle to stop as soon as convenient.
 */
typedef struct ginkgo_sync_client_v1 {
    /* Must equal GINKGO_SYNC_ABI_VERSION; checked before use. */
    uint32_t interface_version;

    char *(*get_state)(void);
    char *(*create_vault)(const char *request_json);
    char *(*join_vault)(const char *request_json);
    int32_t (*run_sync_cycle)(const ginkgo_sync_host_v1 *host);
    void (*cancel)(void);
    void (*free_string)(const char *string);
} ginkgo_sync_client_v1;

/* The entry point's type. */
typedef const ginkgo_sync_client_v1 *(*ginkgo_sync_client_entry_t)(void);

/* Declared here so a C plugin defining it with a mismatched signature
 * fails to compile. Declaring it does not require every includer to
 * define it. */
const ginkgo_sync_client_v1 *ginkgo_sync_client_entry_v1(void);

#ifdef __cplusplus
}
#endif

#endif /* GINKGO_SYNC_ABI_H */
