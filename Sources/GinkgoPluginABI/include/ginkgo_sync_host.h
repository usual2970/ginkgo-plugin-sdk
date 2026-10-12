#ifndef GINKGO_SYNC_ABI_H
#define GINKGO_SYNC_ABI_H

/*
 * Ginkgo encrypted history sync ABI, version 2 (sync-subscription plan U6;
 * v1 was the encrypted-history-sync plan's U4). The companion to
 * ginkgo_plugin.h for the OFFICIAL sync plugin (dev.ginkgo.sync): the
 * plugin owns cryptography and the sync-protocol client; the host owns
 * history storage, scheduling, and Settings UX. This header freezes the
 * seam between them as plain C.
 *
 * Version 2 breaks from 1 (structs and the entry symbol are renamed, never
 * re-used): it adds the web-surface client functions (one-time web links,
 * customer portal, pull-cursor reset, subscription rebind), the host's
 * live-blob listing, per-file plan limits in status/state, and switches the
 * export envelope's "blobs" map from inline base64 bytes to blob id + size
 * — the plugin now reads every attachment through read_blob_for_record
 * (KTD8), which keeps only one attachment in memory at a time.
 *
 * How the two sides meet:
 *   - The plugin bundle is a regular .ginkgoplugin (ginkgo_plugin.h) whose
 *     match/convert are unused (runsOnCopy false). It ADDITIONALLY exports
 *     GINKGO_SYNC_CLIENT_ENTRY, which the host resolves with dlsym — but
 *     ONLY for the allowlisted official id (the host never wires the table
 *     below into any other bundle's code).
 *   - The host builds a ginkgo_sync_host_v2 in memory and hands a pointer
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
 * (payload schemaVersion 1 — unchanged; the envelope spelling here is the
 * decrypted host<->plugin seam, versioned by GINKGO_SYNC_ABI_VERSION).
 * Records are opaque to the SERVER (the plugin encrypts them).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The interface version this header describes. A table whose
 * interface_version differs is rejected before any call: bump only for a
 * breaking change, and never re-use a version number. */
#define GINKGO_SYNC_ABI_VERSION 2

/* The additional symbol the official sync plugin exports (dlsym'd by the
 * host after the bundle passed the standard external-plugin validation).
 * Versioned with the ABI: a v1 plugin exports ..._v1 and is refused with a
 * visible "update the sync plugin" error, not a silent absence. */
#define GINKGO_SYNC_CLIENT_ENTRY ginkgo_sync_client_entry_v2

/* ------------------------------------------------------------------ */
/* Host side: callbacks the host implements.                           */
/* ------------------------------------------------------------------ */

/*
 * export_changes_since returns (heap JSON string, host-owned):
 *   {"records":      [ <sync record DTO>, ... ],  // tombstones included
 *    "blobs":        { "<blobID>": <size bytes>, ... },  // id + size ONLY:
 *                    // no inline bytes (KTD8) — the plugin fetches each
 *                    // attachment via read_blob_for_record after asking
 *                    // the server which are missing
 *    "releasedBlobs": ["<blobID>", ...],  // attachments orphaned by THIS
 *                    // batch's tombstones (no live record references them
 *                    // here); the plugin DELETEs them from the server
 *                    // before pushing the batch (KTD10). Empty when the
 *                    // host is at its record limit (release is a
 *                    // subscriber feature, KTD10) or nothing was released.
 *    "nextRevision":  <int>}             // cursor to persist
 * or NULL on failure.
 *
 * apply_changes takes
 *   records_json: {"records": [ <sync record DTO>, ... ]}  (no bytes)
 *   blobs_json:   { "<blobID>": "<base64 plaintext>", ... }
 * and returns {"applied":N,"stale":N,"unknownTombstones":N} or NULL.
 * Idempotent under LWW; applying a batch twice is always safe. On pull the
 * plugin applies record-by-record (or byte-budgeted small batches, KTD8):
 * one attachment's plaintext is in memory at a time.
 *
 * read_blob_for_record returns
 *   {"data": "<base64>" | null, "displayName": "..."}   // null = file
 *                                                      // unavailable
 * or NULL when the record / index is unknown. file_index < 0 addresses
 * the record's image bytes. In v2 this is the ONLY host→plugin byte
 * channel: the export envelope carries ids and sizes, and the plugin reads
 * each attachment through here (encrypt, upload; or decrypt+deliver on
 * pull it already holds as base64 from its own download).
 *
 * notify_status receives the plugin's progress/state JSON (documented in
 * sync-protocol.md); the host surfaces it in Settings. The FIRST
 * notify_status of every cycle carries
 *   {"phase":"limits","perFileLimitBytes":N}
 * — the plan's per-attachment cap the host must export against (KTD6);
 * before the first status of a session the host uses its 100 MB hard cap.
 *
 * list_live_blob_ids (v2, KTD10's daily reconciliation) returns
 *   {"blobIds": ["<hex sha-256>", ...]}
 * — every attachment id referenced by the host's LIVE (non-tombstoned)
 * records. The plugin asks the server which of these are missing and
 * re-uploads them from local bytes, so one device's released blob is not
 * lost while another device's live records still reference it. NULL on
 * failure.
 */
typedef struct ginkgo_sync_host_v2 {
    /* Must equal GINKGO_SYNC_ABI_VERSION; checked before use. */
    uint32_t interface_version;

    /* Opaque host state; passed back as the first argument of every
     * callback. The plugin never interprets or stores it. */
    void *context;

    char *(*export_changes_since)(void *context, int64_t since_revision, int32_t limit);
    char *(*apply_changes)(void *context, const char *records_json, const char *blobs_json);
    char *(*read_blob_for_record)(void *context, const char *sync_record_id, int32_t file_index);
    void (*notify_status)(void *context, const char *status_json);
    char *(*list_live_blob_ids)(void *context);
    void (*free_string)(void *context, char *string);
} ginkgo_sync_host_v2;

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
 *    "quota": {"usedBytes":N,"limitBytes":N,"remainingBytes":N},
 *    "subscription": {"status":"...","plan":"...",
 *                     "deleteAt":<unix seconds>,   // grace-period end when
 *                                                  // not "effective"
 *                     "perFileLimitBytes":N}}      // plan's attachment cap
 * "subscription" is present only while the vault is bound to one (free
 * installs and unbound vaults omit it; deleteAt is then absent too, R3).
 *
 * create_vault / join_vault take
 *   {"passphrase":"...","deviceName":"...",           // create
 *    "subscriptionKey":"...","replaceExisting":false} // create only; the
 *        // key is request-ONLY — validated with the server, never
 *        // persisted plugin-side (KTD4 binds it server-side to the vault)
 *   {"code":"...","passphrase":"...","deviceName":"..."}  // join
 * and return the get_state shape on success, or
 *   {"error":"<machine-readable>","message":"<human>"}  on failure.
 * Without replaceExisting, a subscriptionKey already bound to another LIVE
 * vault fails {"error":"vaultExists"} (409) — the host then explains the
 * replacement and re-calls create_vault with replaceExisting=true (R27);
 * the replaced vault stops syncing and is kept 30 days, swap-back-able.
 * Keys, tokens, and the export cursor persist in the plugin's own state
 * (never the pasteboard, never the host's history store; the subscription
 * key is in neither).
 *
 * mint_pairing_code takes
 *   {"passphrase":"..."}   // the vault passphrase, re-entered to wrap
 *                          // the vault key for the joining device
 * and returns the get_state shape PLUS
 *   {"pairingCode":"<8 chars>","expiresAt":<unix seconds>}
 * or the error shape. Only valid while configured; the code is
 * single-use and short-lived (see sync-protocol.md).
 *
 * mint_web_link (v2, R19) takes a reserved empty request "{}" and returns
 *   {"code":"<one-time code>","url":"https://<site>/account?code=...",
 *    "expiresAt":<unix seconds>}
 * or the error shape. The host opens the URL in the browser; the site
 * trades the code for a 30-day cookie session (stats + portal). The code
 * is single-use, 5 minutes, and held only in memory en route to the
 * browser — never persisted or logged.
 *
 * open_billing_portal (v2, R28) takes a reserved empty request "{}" and
 * returns {"url":"<Dodo customer-portal session>"} for the host to open,
 * or the error shape ("billing surface disabled" while the server's billing
 * is unconfigured; "not found" while the vault has no bound subscription).
 *
 * reset_pull_cursor (v2, KTD11) takes a reserved empty request "{}",
 * sets the plugin's pull cursor to 0 (the NEXT cycle re-pulls everything;
 * LWW makes the replay idempotent), and returns {"ok":true}. The host calls
 * it once after the user activates a license, so records previously dropped
 * at the free tier's 50-record pull cap are fetched. Unconfigured vaults
 * return the error shape.
 *
 * rebind_subscription (v2, R27) takes
 *   {"subscriptionKey":"..."}
 * and returns the get_state shape on success (the vault is bound to the
 * key's subscription; quota and limits refresh), or the error shape —
 * including {"error":"vaultExists"} when the key is bound to another LIVE
 * vault (the host then offers the replacement flow via create_vault with
 * replaceExisting=true), and {"error":"invalidSubscriptionKey"} for a key
 * the server does not know. The key is request-only, never persisted.
 *
 * run_sync_cycle performs ONE full pass — per sync-protocol.md v2: notify
 * plan limits, pull then push (KTD10's pull-before-push so releases free
 * quota before the push), DELETE releasedBlobs, reconcile missing blobs,
 * upload, push — returning 0 on success and non-zero on failure. It must
 * tolerate being called with no vault configured (a no-op success).
 *
 * cancel asks the in-flight cycle to stop as soon as convenient.
 */
typedef struct ginkgo_sync_client_v2 {
    /* Must equal GINKGO_SYNC_ABI_VERSION; checked before use. */
    uint32_t interface_version;

    char *(*get_state)(void);
    char *(*create_vault)(const char *request_json);
    char *(*join_vault)(const char *request_json);
    char *(*mint_pairing_code)(const char *request_json);
    char *(*mint_web_link)(const char *request_json);
    char *(*open_billing_portal)(const char *request_json);
    char *(*reset_pull_cursor)(const char *request_json);
    char *(*rebind_subscription)(const char *request_json);
    int32_t (*run_sync_cycle)(const ginkgo_sync_host_v2 *host);
    void (*cancel)(void);
    void (*free_string)(const char *string);
} ginkgo_sync_client_v2;

/* The entry point's type. */
typedef const ginkgo_sync_client_v2 *(*ginkgo_sync_client_entry_t)(void);

/* Declared here so a C plugin defining it with a mismatched signature
 * fails to compile. Declaring it does not require every includer to
 * define it. */
const ginkgo_sync_client_v2 *ginkgo_sync_client_entry_v2(void);

#ifdef __cplusplus
}
#endif

#endif /* GINKGO_SYNC_ABI_H */
