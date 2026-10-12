#ifndef GINKGO_PLUGIN_ABI_H
#define GINKGO_PLUGIN_ABI_H

/*
 * Ginkgo external plugin interface, version 1 (external-plugin-runtime plan
 * KTD2). The contract between the Ginkgo host and a native `.ginkgoplugin`
 * bundle, frozen as plain C so a plugin compiled today keeps loading across
 * Ginkgo releases and toolchain changes.
 *
 * Bundle contract (summary — host copy: ginkgo/docs/external-plugins.md;
 * author-facing copy: ginkgo-plugin-sdk docs/external-plugins.md; keep
 * both trees byte-identical via ginkgo-workspace/script/verify-plugin-abi-sync.sh):
 *   - The bundle exports the entry symbol named by GINKGO_PLUGIN_ENTRY.
 *     The host resolves it with dlsym after dlopen and calls it ONCE; it
 *     must return a pointer to a static, immortal function table whose
 *     interface_version equals GINKGO_PLUGIN_INTERFACE_VERSION.
 *   - match/convert take one UTF-8 JSON request string and return either
 *     NULL (no match / nothing to say) or a heap string the plugin owns
 *     until the host passes it back through free_string. The host copies
 *     before freeing; a returned pointer is never used after that.
 *   - Every function in the table may be called from any thread, possibly
 *     concurrently: implementations must be thread-safe and reentrant.
 *     match must be fast (a fraction of the host's 0.5 s match budget);
 *     convert may take up to the host's convert deadline.
 *   - A plugin NEVER writes the system pasteboard. Adopt goes through the
 *     host. The host cannot enforce this; the manifest's grant flow is
 *     the user's consent to declared behavior, not a sandbox.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The interface version this header describes. A table whose
 * interface_version differs is rejected at load: bump only for a
 * breaking change, and never re-use a version number. */
#define GINKGO_PLUGIN_INTERFACE_VERSION 1

/* The symbol every plugin bundle must export (the host dlsym's exactly
 * this name). */
#define GINKGO_PLUGIN_ENTRY ginkgo_plugin_entry_v1

/* Request and response strings: NUL-terminated UTF-8 JSON.
 *
 * match request:  {"text": "<the copied text>"}
 * match response: {"title": "...", "body": "...", "adoptedOutput": "..."}
 *                 or NULL for "this plugin has nothing to say".
 * convert request:  {"text": "...", "parameter": "<chosen candidate id>"}
 * convert response: the success shape above, or
 *                 {"error": "failed" | "inputTooLarge", "reason": "<optional>"}.
 *                 When "reason" is present the host maps known values to typed
 *                 conversion hints (e.g. sourceLanguageUndetected,
 *                 alreadyInTargetLanguage, languagePackMissing,
 *                 unsupportedLanguagePair); unknown reasons collapse to failed.
 *
 * "title" is the short headline; "body" the insight text shown beside the
 * clip; "adoptedOutput" what Adopt writes (defaults honored by the host
 * when omitted is NOT provided in v1 — include all three keys). */
typedef struct ginkgo_plugin_functions {
    /* Must equal GINKGO_PLUGIN_INTERFACE_VERSION; checked before use. */
    uint32_t interface_version;

    /* Copy-time inspection. NULL inside the table is invalid for v1. */
    const char *(*match)(const char *request_json);

    /* On-demand conversion. NULL when the manifest declares no command
     * parameter; the host never calls it otherwise. */
    const char *(*convert)(const char *request_json);

    /* Releases a string this plugin returned. The host calls it exactly
     * once per returned non-NULL string, then drops the pointer. */
    void (*free_string)(const char *string);
} ginkgo_plugin_functions;

/* The entry point's type. */
typedef const ginkgo_plugin_functions *(*ginkgo_plugin_entry_t)(void);

/* Declared here so a C plugin defining it with a mismatched signature
 * fails to compile. Declaring it does not require every includer to
 * define it. */
const ginkgo_plugin_functions *ginkgo_plugin_entry_v1(void);

#ifdef __cplusplus
}
#endif

/* Sync host/client vtables (dev.ginkgo.sync). Pulled in here so SwiftPM's
   GinkgoPluginABI clang module always exports ginkgo_sync_host_v2 to Swift
   importers, not only to C translation units that include this file. */
#include "ginkgo_sync_host.h"

#endif /* GINKGO_PLUGIN_ABI_H */
