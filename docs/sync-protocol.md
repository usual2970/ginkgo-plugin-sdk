# Ginkgo Sync Protocol (ABI v2)

Authoritative wire format for the OFFICIAL sync plugin (`dev.ginkgo.sync`,
sync ABI v2). The plugin owns this protocol; the host only speaks the
decrypted record spelling of `ginkgo_sync_host.h`. Everything here is
versioned by `schemaVersion: 1` (payload plaintext — unchanged since v1)
and `GINKGO_SYNC_ABI_VERSION = 2` (the C seam; v1 structs and symbols were
renamed, never re-used).

The server (`ginkgo-api` `/v1/sync/...`) stores ciphertext only: no field
below marked *ciphertext* is inspectable server-side, and no key, plaintext
code, or subscription key ever reaches it in the clear over the wire
(HTTPS) or at rest.

## Transport

| Route | Body / query | Response |
| --- | --- | --- |
| `POST /v1/sync/vaults` | `{"deviceName","devicePublicKey","subscriptionKey"?,"replaceExisting"?}` | `201 {"vaultId","deviceId","token","quotaBytes","perFileLimitBytes"}` |
| `POST /v1/sync/vaults/join` | `{"code","deviceName","devicePublicKey"}` | `200 {"vaultId","deviceId","token","encryptedKeyMaterial","quotaBytes"}` |
| `POST /v1/sync/vaults/{id}/pairing-codes` | `{"codeHash","encryptedKeyMaterial","ttlSeconds"}` | `201 {"expiresAt"}` |
| `GET /v1/sync/vaults/{id}/changes?cursor&limit` | — | `200 {"changes":[{"id","deviceId","payload","createdAt"}],"nextCursor"}` |
| `POST /v1/sync/vaults/{id}/changes` | `{"payload"}` | `201 {"id","usedBytes","limitBytes"}` |
| `PUT /v1/sync/vaults/{id}/blobs/{blobId}` | body = raw ciphertext, `Content-Length` required | `200 {"usedBytes","limitBytes"}` |
| `GET /v1/sync/vaults/{id}/blobs/{blobId}` | — | `200` raw ciphertext |
| `DELETE /v1/sync/vaults/{id}/blobs/{blobId}` | — | `200` (soft delete; see Blob lifecycle) |
| `POST /v1/sync/vaults/{id}/blobs/missing` | `{"blobIds":[...]}` | `200 {"missing":[...]}` |
| `GET /v1/sync/vaults/{id}/quota` | — | `200 {"usedBytes","limitBytes","remainingBytes","perFileLimitBytes","deleteAt"?,"subscription"?}` |
| `GET /v1/sync/vaults/{id}/devices` | — | `200 {"devices":[...]}` |
| `POST /v1/sync/vaults/{id}/subscription` | `{"subscriptionKey"}` | `200 {"usedBytes","limitBytes","perFileLimitBytes"}` |
| `POST /v1/sync/vaults/{id}/billing-portal` | `{}` | `200 {"url"}` |
| `POST /v1/sync/vaults/{id}/web-links` | `{}` | `201 {"code","url"?,"expiresAt"}` |

Auth: `Authorization: Bearer <device token>` on every route except
create/join. Errors: `{"error":"<machine-readable>"}` with
400/401/402/409/410/413/503:

- `401 vaultDeleted` — the vault was purged; the ONLY code that clears the
  plugin's local vault configuration.
- `402 subscriptionInactive` (+ `deleteAt`) — the bound subscription
  lapsed; stop the cycle, keep the configuration, surface `deleteAt` (R7).
- `402 vaultReplaced` (+ `deleteAt`) — this vault was replaced on its
  subscription; same handling, swap-back-able for 30 days (R27).
- `409 vaultExists` — the subscription key is bound to another live vault;
  the host explains the replacement and may re-call `create_vault` with
  `replaceExisting: true` (R27).
- `400 invalidSubscriptionKey` — unknown key.
- `413 quotaExceeded` / `413 attachmentOverLimit` — see the cycle rules.
- `503 billing surface disabled` — the server's billing is unconfigured.

Default base URL `https://api.ginkgo.ink` (same origin as the
marketplace; moves with it).

## Cryptography

- **Vault key**: 32 random bytes (`SecRandomCopyBytes`), generated once per
  vault by the creating device. Stored only in the local keychain of each
  member device.
- **Record batches**: AES-256-GCM under the vault key, combined
  representation `nonce(12) || ciphertext || tag(16)`; the base64 of that
  string is the `payload` the server stores.
- **Blobs**: the same AES-256-GCM combined representation, raw bytes as the
  PUT body. `blobId` = lowercase hex SHA-256 of the PLAINTEXT (both devices
  derive the same id; the server never sees plaintext).
- **Device identity**: Ed25519 (`Curve25519.Signing`) keypair per install,
  private half in the keychain. `devicePublicKey` is the base64 raw
  representation (32 bytes) presented at create/join — the server records it
  for audit; v1 does not yet sign changes (follow-up hardening).
- **Passphrase KDF**: Argon2id (RFC 9106, type 2, version 0x13), parameters
  `m=65536 KiB, t=3, p=4` (the RFC's second recommended option). Only used
  to wrap the vault key inside `encryptedKeyMaterial`.

### Key wrap (`encryptedKeyMaterial`, ≤ 16 KB)

What travels inside pairing-code escrow so a joining device can obtain the
vault key — base64 of:

```json
{"v": 1,
 "kdf": {"algo": "argon2id", "mKiB": 65536, "iterations": 3,
         "parallelism": 4, "salt": "<b64 16B>", "codeSalt": "<b64 32B>"},
 "ciphertext": "<b64 AES-256-GCM combined (nonce‖ct‖tag) of the 32-byte vault key>"}
```

The wrap key is `Argon2id(password = utf8(passphrase),
salt = salt || codeSalt)`. `codeSalt` is SHA-256 of
`"ginkgo-sync-pairing:" + pairingCode` — so unwrapping needs the passphrase
AND the single-use code the minting device displayed out-of-band (KTD3:
passphrase-wrapped escrow, code-derived salt, neither alone sufficient).
`salt` is random per mint. A wrong passphrase or wrong code fails GCM
authentication — the two are indistinguishable to the joiner.

### Pairing codes

Minted client-side: 8 chars from `ABCDEFGHJKMNPQRSTUVWXYZ23456789`
(unambiguous subset of the server's `[A-Za-z0-9]{8}`). `codeHash` is
lowercase hex SHA-256 of the code. TTL 300 s, single use; the plaintext code
is displayed by the minting device's host UI only.

## Record payload (the decrypted `payload` plaintext)

```json
{"schemaVersion": 1,
 "deviceId": "<writer device uuid>",
 "records": [ <sync record DTO>, ... ],
 "blobs": ["<blobId>", ...]}
```

- `records` are the host's `ginkgo_sync_host.h` sync record DTOs verbatim
  (tombstones included), minus blob bytes: records reference `blobId`s,
  bytes ride in the blob store. Batches cap at 100 records; a device pushes
  repeatedly until its export cursor drains.
- `blobs` lists the blob ids uploaded alongside this batch (each id PUT once
  per device lifetime; content addressing dedupes across devices). An
  attachment refused `413 attachmentOverLimit` is dropped from this list and
  the batch still pushes (KTD6) — the record travels metadata-only.
- Receivers skip changes whose `deviceId` equals their own.
- LWW (`syncUpdatedAt`, then `deviceID` tie-break) lives in the HOST; the
  plugin just applies batches idempotently.

## Host ↔ plugin attachment channel (v2, KTD8)

The v2 export envelope (`export_changes_since`) carries attachment ids and
SIZES only — never inline bytes:

```json
{"records": [...],
 "blobs": {"<blobId>": <plaintext size in bytes>, ...},
 "releasedBlobs": ["<blobId>", ...],
 "nextRevision": 42}
```

- The plugin asks the server (`POST blobs/missing`) which ids the vault
  lacks, then reads each needed attachment through the existing
  `read_blob_for_record`, encrypts, and PUTs it — one attachment's
  plaintext in memory at a time.
- `releasedBlobs` lists attachments orphaned by THIS batch's tombstones
  (computed by the host against its live records, KTD10). The plugin
  DELETEs each before pushing the batch, so an over-quota vault frees space
  first; a failed DELETE retries next cycle.
- On pull, the plugin downloads each foreign attachment once, decrypts, and
  feeds `apply_changes` record-by-record (or byte-budgeted small batches) —
  never a whole 100-record batch's attachments at once. A 404 on download
  applies the record as "unavailable" and moves the cursor on (KTD10).

## Sync cycle (`run_sync_cycle`, v2 order)

One pass, returning 0 on success:

1. **Limits**: `notify_status {"phase":"limits","perFileLimitBytes":N}` —
   the plan's per-attachment cap the host must export against (KTD6); the
   host also takes it from `create_vault`/`join_vault` state responses.
2. **Pull** (KTD10's pull-before-push): `GET changes?cursor=pullCursor`
   until a page is empty; per foreign change, fetch + decrypt + apply as
   above, advancing `pullCursor` per page.
3. **Export**: `export_changes_since(lastExportRevision, 100)` until a
   batch is empty.
4. **Release**: DELETE each of the batch's `releasedBlobs` (KTD10) — before
   the push, so over-quota vaults free space first. Failed ids retry next
   cycle.
5. **Missing**: `POST blobs/missing` with the batch's blob ids; only the
   answer's ids are uploaded (never trust the local uploaded cache alone).
6. **Upload**: for each missing id, `read_blob_for_record` → encrypt →
   PUT. `413 attachmentOverLimit` skips that id (drop from the batch's
   `blobs` list, keep pushing — the record travels metadata-only, KTD6).
   `413 quotaExceeded` ends the push: the batch is NOT pushed, the export
   cursor does NOT move, status reports over-quota; the batch re-uploads
   after space frees.
7. **Push**: POST the sealed payload; on 201 advance `lastExportRevision`
   to the batch's `nextRevision` and persist it (a crash mid-batch at
   worst re-pushes idempotently).
8. **Quota**: `GET quota` → cache for `get_state`.
9. `notify_status` with `{"phase":"done","pushed":N,"pulled":N,"applied":N}`
   (or `{"phase":"failed","error":"..."}`); the state file persists cursors,
   `lastSyncAt`, `lastError`.

`402` responses stop the cycle (see Transport), keeping the local
configuration; only `401 vaultDeleted` clears it. `cancel` stops between
batches and blob uploads; partial progress is kept.

### Daily reconciliation (KTD10)

At least once a day the plugin calls the host's `list_live_blob_ids`
(`{"blobIds":[...]}` — every attachment its LIVE records still reference),
asks `missing`, and re-uploads whatever the vault lacks from local bytes —
so a blob device A released cannot be lost while device B's live records
still reference it.

## `get_state` (v2)

```json
{"configured": <bool>,
 "vaultId": "<uuid>", "deviceId": "<uuid>", "deviceName": "...",
 "lastSyncAt": <unix seconds>,      // after a first successful pass
 "lastError": "...",                // while the last pass failed
 "quota": {"usedBytes":N,"limitBytes":N,"remainingBytes":N},
 "subscription": {"status":"...","plan":"...",
                  "deleteAt":<unix seconds>, "perFileLimitBytes":N}}
```

`subscription` is present only while the vault is bound to one — free
installs and unbound vaults omit it (R3), and `deleteAt` rides inside it
while the subscription is not effective (R8).

Plus, on `mint_pairing_code` success only:
`{"pairingCode":"<8 chars>","pairingExpiresAt":<unix seconds>}`.

### v2 client functions (KTD12)

- `mint_web_link("{}")` → `{"code":"<one-time>","url":"<site>/account?code=…","expiresAt":<unix>}`.
  The host opens the URL; the site trades the code for a 30-day session.
  The code is single-use, 5 minutes, memory-only (R19).
- `open_billing_portal("{}")` → `{"url":"<Dodo portal session>"}` for the
  host to open (R28).
- `reset_pull_cursor("{}")` → `{"ok":true}`; the next cycle re-pulls from
  0 — the host calls it once after a license activation so records dropped
  at the free tier's pull cap are fetched; LWW makes the replay idempotent
  (KTD11).
- `rebind_subscription({"subscriptionKey":"..."})` → the `get_state` shape,
  or the error shape (`vaultExists`, `invalidSubscriptionKey`). The key is
  request-ONLY: validated server-side, never persisted in the state file
  or keychain, never logged.

`create_vault` additionally accepts `subscriptionKey` and
`replaceExisting` (same request-only rule); failures return
`{"error":"<machine-readable>","message":"<human>"}`.

## Plugin-side persistence

- Keychain (service `dev.ginkgo.sync.v1`): vault key, device token, Ed25519
  seed. Never in files, never in the host's stores.
- `~/Library/Application Support/Ginkgo/PluginState/dev.ginkgo.sync/state.json`:
  vault/device ids and names, export + pull cursors, uploaded blob ids,
  quota cache, `lastSyncAt`/`lastError`. Never the subscription key (it is
  request-only), never a passphrase or vault key. Deleting this folder
  (plus the keychain items) is the plugin's full uninstall footprint.
