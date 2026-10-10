# Ginkgo Sync Protocol v1

Authoritative wire format for the OFFICIAL sync plugin (`dev.ginkgo.sync`,
sync ABI v1). The plugin owns this protocol; the host only speaks the
decrypted record spelling of `ginkgo_sync_host.h`. Everything here is
versioned by `schemaVersion: 1` (payload plaintext) and
`GINKGO_SYNC_ABI_VERSION` (the C seam).

The server (`ginkgo-api` `/v1/sync/...`, sync plan KTD9) stores ciphertext
only: no field below marked *ciphertext* is inspectable server-side, and no
key or plaintext code ever reaches it.

## Transport

| Route | Body / query | Response |
| --- | --- | --- |
| `POST /v1/sync/vaults` | `{"deviceName","devicePublicKey"}` | `201 {"vaultId","deviceId","token","quotaBytes"}` |
| `POST /v1/sync/vaults/join` | `{"code","deviceName","devicePublicKey"}` | `200 {"vaultId","deviceId","token","encryptedKeyMaterial","quotaBytes"}` |
| `POST /v1/sync/vaults/{id}/pairing-codes` | `{"codeHash","encryptedKeyMaterial","ttlSeconds"}` | `201 {"expiresAt"}` |
| `GET /v1/sync/vaults/{id}/changes?cursor&limit` | — | `200 {"changes":[{"id","deviceId","payload","createdAt"}],"nextCursor"}` |
| `POST /v1/sync/vaults/{id}/changes` | `{"payload"}` | `201 {"id","usedBytes","limitBytes"}` |
| `PUT /v1/sync/vaults/{id}/blobs/{blobId}` | body = raw ciphertext | `200 {"usedBytes","limitBytes"}` |
| `GET /v1/sync/vaults/{id}/blobs/{blobId}` | — | `200` raw ciphertext |
| `GET /v1/sync/vaults/{id}/quota` | — | `200 {"usedBytes","limitBytes","remainingBytes"}` |

Auth: `Authorization: Bearer <device token>` on every route except
create/join. Errors: `{"error":"<machine-readable>"}` with 400/401/410/413.

Default base URL `https://ginkgo-api.ikit.fun` (same origin as the
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
  per device lifetime; content addressing dedupes across devices).
- Receivers skip changes whose `deviceId` equals their own.
- LWW (`syncUpdatedAt`, then `deviceID` tie-break) lives in the HOST; the
  plugin just applies batches idempotently.

## Sync cycle (`run_sync_cycle`)

One pass, returning 0 on success:

1. **Push**: `export_changes_since(lastExportRevision, 100)` until a batch
   comes back empty. For each batch: PUT any not-yet-uploaded blobs, then
   POST the sealed payload. On 201, advance `lastExportRevision` to the
   batch's `nextRevision` and persist it (a crash mid-batch at worst
   re-pushes idempotently).
2. **Pull**: `GET changes?cursor=pullCursor` until a page is empty. For
   each foreign change: open the payload, GET missing blobs, decrypt, then
   `apply_changes(records, blobs)`. Advance `pullCursor` per applied page.
3. **Quota**: `GET quota` → cache for `get_state`.
4. `notify_status` with `{"phase":"done","pushed":N,"pulled":N,"applied":N}`
   (or `{"phase":"failed","error":"..."}`); state file persists cursors,
   `lastSyncAt`, `lastError`.

Quota (413): fail the cycle without advancing cursors — existing data stays
readable, the new upload is refused (R15/R16). `cancel` stops between
batches and blob uploads; partial progress is kept.

## `get_state`

```json
{"configured": <bool>,
 "vaultId": "<uuid>", "deviceId": "<uuid>", "deviceName": "...",
 "lastSyncAt": <unix seconds>,      // after a first successful pass
 "lastError": "...",                // while the last pass failed
 "quota": {"usedBytes":N,"limitBytes":N,"remainingBytes":N}}
```

Plus, on `mint_pairing_code` success only:
`{"pairingCode":"<8 chars>","pairingExpiresAt":<unix seconds>}`.

`create_vault` / `join_vault` / `mint_pairing_code` failures return
`{"error":"<machine-readable>","message":"<human>"}`.

## Plugin-side persistence

- Keychain (service `dev.ginkgo.sync.v1`): vault key, device token, Ed25519
  seed. Never in files, never in the host's stores.
- `~/Library/Application Support/Ginkgo/PluginState/dev.ginkgo.sync/state.json`:
  vault/device ids and names, export + pull cursors, uploaded blob ids,
  quota cache, `lastSyncAt`/`lastError`. Deleting this folder (plus the
  keychain items) is the plugin's full uninstall footprint.
