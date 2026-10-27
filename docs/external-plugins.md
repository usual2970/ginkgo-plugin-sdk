# External plugins

Ginkgo loads native plugin bundles dropped into
`~/Library/Application Support/Ginkgo/Plugins/` — one `.ginkgoplugin`
directory per plugin. There is no marketplace and no in-app installer:
installing a plugin is copying a folder, and enabling one is an explicit
Settings act with per-capability consent.

This is the authoritative interface reference for the host tree. **Third-
party authors** should use the public
[`ginkgo-plugin-sdk`](https://github.com/usual2970/ginkgo-plugin-sdk)
repository (`docs/external-plugins.md` there) — no access to this private
repo is required. Sample bundles for authors with workspace access live in
the `plugins/` submodule (`plugins/README.md`).

## Bundle layout

```
HexColor.ginkgoplugin/
  Contents/
    Info.plist                 CFBundleExecutable (+ the usual BNDL keys)
    MacOS/HexColor             the plugin binary (dylib or bundle executable)
    Resources/manifest.json    validated BEFORE any code runs
```

## The manifest

`Contents/Resources/manifest.json` — all fields optional at the JSON
level so the validator can name WHICH field failed, but with required
values as below. `schemaVersion` and `interfaceVersion` must match what
this host understands (currently `1`); a newer plugin cannot slip past
an older host.

```json
{
  "schemaVersion": 1,
  "id": "sample.ginkgo.hexcolor",
  "version": "1.0.0",
  "interfaceVersion": 1,
  "name": "Hex Color",
  "nameZhHans": "十六进制颜色",
  "description": "Describes hex colors — the decoded RGB at a glance.",
  "descriptionZhHans": "解读十六进制颜色——一眼看到解码后的 RGB。",
  "runsOnCopy": true,
  "capabilities": ["network"],
  "commandParameter": {
    "label": "Case",
    "labelZhHans": "大小写",
    "candidates": [
      { "id": "upper", "name": "UPPERCASE", "nameZhHans": "全部大写" }
    ],
    "defaultCandidateID": "upper"
  }
}
```

- `id` — reverse-DNS, unique across the folder, never the reserved
  `builtin.` prefix (built-ins own it).
- `name` / `description` — English, required. The `…ZhHans` twins are
  optional; the UI falls back to English.
- `runsOnCopy` — `true` for the copy-time match pass (default), `false`
  for on-demand-only plugins.
- `capabilities` — a subset of `network`, `clipboardRead`, `files`,
  `historyAppend`. Declaring one is not a feature request: it is what
  the Settings grant block asks the user to consent to before the switch
  passes. A grant is **consent, not enforcement** — Ginkgo cannot
  technically restrict what a plugin does, and an update that declares
  more than was granted forces the plugin back off until re-consented.
  `historyAppend` additionally requires the bundle to export
  `ginkgo_plugin_bind_host_v1` (see History append below); missing that
  symbol rejects the package at load.
- `commandParameter` — declares an on-demand conversion with a parameter
  (the History `/` command list shows the candidates); requires a
  `convert` entry in the table.

Validation order (each failure has its own typed reason on the Settings
page): manifest readable → schema/interface versions → id rules →
required strings → capabilities → parameter → executable present →
architecture → code signature → quarantine → crash sentinel → load.

## The C interface (version 1)

SwiftPM product **`GinkgoPluginABI`**: in this workspace, the
`ginkgo/GinkgoPluginABI` target; for external authors, the public
**ginkgo-plugin-sdk** package (see its README). C/Rust plugins include
`ginkgo_plugin.h` and export `ginkgo_plugin_entry_v1`. A Swift plugin
writes the entry with `@_cdecl`:

```swift
import GinkgoPluginABI

@_cdecl("ginkgo_plugin_entry_v1")
public func ginkgo_plugin_entry_v1() -> UnsafePointer<ginkgo_plugin_functions>? {
    let table = ginkgo_plugin_functions(
        interface_version: 1,                    // #define invisible to Swift
        match: { MyPlugin.matchImpl($0) },       // literal closures: @convention(c)
        convert: nil,                            // nil unless a commandParameter is declared
        free_string: { MyPlugin.freeImpl($0) }
    )
    let pointer = UnsafeMutablePointer<ginkgo_plugin_functions>.allocate(capacity: 1)
    pointer.initialize(to: table)
    return UnsafeRawPointer(pointer).assumingMemoryBound(to: ginkgo_plugin_functions.self)
}
```

The host `dlopen`s the bundle's executable, `dlsym`s
`ginkgo_plugin_entry_v1`, and calls it ONCE. It must return a pointer to
a static, immortal table whose `interface_version` equals
`GINKGO_PLUGIN_INTERFACE_VERSION` — anything else is rejected at load.
The host copies the table struct and never `dlclose`s: your code stays
mapped for the process lifetime, and a changed binary is only picked up
after a relaunch (Settings flags it with a relaunch hint).

### Requests and responses

`match` and `convert` take one NUL-terminated UTF-8 JSON string and
return either NULL ("nothing to say") or a heap string you own until the
host passes it back through your `free_string` — exactly once per
returned pointer, before any decoding, so a malformed response is still
released exactly once.

- match request: `{"text": "<the copied text>"}` — the host may also send optional keys such as `"pair"` (curated Dictionary language-pair id). Plugins that only read `text` ignore unknown keys; additive optional request fields do not bump `interface_version`.
- match response: `{"title": "...", "body": "...", "adoptedOutput": "..."}`
  (include all three keys; `adoptedOutput` is what Adopt writes)
- convert request: `{"text": "...", "parameter": "<chosen candidate id>"}`
- convert response: the success shape, or `{"error": "failed"}` /
  `{"error": "inputTooLarge"}`. Optional `"reason"` (string) maps to typed
  hints when recognized (`sourceLanguageUndetected`, `alreadyInTargetLanguage`,
  `languagePackMissing`, `unsupportedLanguagePair`, `inputTooLarge`); unknown
  values collapse to a generic failure.

## History append (`historyAppend`)

Optional side vtable in `ginkgo_history_host.h` (included from
`ginkgo_plugin.h`). `GINKGO_PLUGIN_INTERFACE_VERSION` stays 1;
`ginkgo_plugin_functions` is unchanged. Mirrors the sync host pattern.

1. Manifest declares `"capabilities": ["historyAppend"]` (raw value
   `historyAppend` / `GINKGO_CAPABILITY_HISTORY_APPEND`).
2. Bundle exports `ginkgo_plugin_bind_host_v1`
   (`GINKGO_PLUGIN_BIND_HOST`). Declared capability without the symbol
   fails load. Symbol without the capability is never bound.
3. After the user grants `historyAppend`, the host builds a
   `ginkgo_history_host_v1` and calls the bind entry once. Store the
   pointer; call `create_clip` later; release responses with
   `free_string`.
4. `create_clip` is only available during `convert`. During `match`,
   outside either phase, or while another `create_clip` is on the stack,
   the host returns `{"error":"unavailable"}`. A missing grant returns
   `{"error":"notGranted"}`.

Request JSON (caller-owned for the call):

```json
{"kind":"text","text":"standup notes","title":"Standup","tags":["notes"]}
```

`kind` is `text` or `image` (`imageBase64` for image bytes). `fileURLs`
is refused in v1 (`{"error":"empty"}`). Optional `title` and `tags`.

Success: `{"id":"<object-uri>","created":true|false}` — `created` is
false when a live row with the same content hash was refreshed.
Refusal: `{"error":"empty"|"insertLimit"|"saveFailed"|"malformed"|"unavailable"|"notGranted"}`.

The host never writes the pasteboard for this path. `match` stays a pure
read even when the plugin holds a bound host pointer.

## Contracts the host relies on

- **Threading:** every table function may be called from any thread,
  possibly concurrently. Implementations must be thread-safe and
  reentrant. `match` must be fast — it runs inside the host's 0.5 s
  copy-time budget; `convert` may use the host's convert deadline.
- **No pasteboard writes, ever:** a plugin NEVER writes the system
  pasteboard. Adopt goes through the host, which carries its own
  write-marker so nothing loops back into history. The host cannot
  enforce this; the manifest's grant flow is consent, not a sandbox.
  History append uses the host's `create_clip` instead of the pasteboard.
- **Privacy:** clipboard content reaches your process only after the
  user has enabled your plugin — which, for anything declared, means
  they have granted it. Nothing else about the user's clipboard leaves
  the device except through your declared capabilities.

## Load-time protections

- **Signature:** the bundle must carry a valid code signature — ad-hoc
  accepted. A byte edited after signing is a rejection on the next scan.
- **Quarantine:** a downloaded bundle carries `com.apple.quarantine`;
  the host refuses it until YOU remove the attribute
  (`xattr -dr com.apple.quarantine <bundle>`) — clearing it is the
  user's deliberate trust step, never the host's.
- **Crash sentinel:** the id of the package being loaded is persisted
  before its code runs and cleared right after. A crash in that window
  means the next launch skips the package with a "crashed while
  loading" rejection — no crash loops — until a Settings Rescan (the
  user's deliberate "try again").

## Release builds and library validation

Release builds are signed with the hardened runtime, which by default
refuses to load libraries not signed by the app's own team — exactly
what a third-party plugin is. `Ginkgo.entitlements` carries
`com.apple.security.cs.disable-library-validation` for this reason; the
host's own per-bundle validation (signature, quarantine, architecture,
sentinel) remains the trust boundary.

Verified locally with an ad-hoc stand-in for the release identity
(2026-10): a hardened-runtime probe without the entitlement refuses a
sample bundle ("different Team IDs" — an ad-hoc host has no team at
all, the strictest case), re-signed with the entitlement it loads the
bundle and its entry symbol, and a bundle with one corrupted byte
inside a code slice is still killed by the kernel (SIGKILL) even with
the entitlement — opting out of library validation does not opt out of
tamper detection. Note dyld only judges the slice it actually maps: a
byte poked into a fat-binary alignment gap loads fine, which the
host's own scan-time signature check catches. The Developer ID +
notarized combination itself remains smoke item 85.
