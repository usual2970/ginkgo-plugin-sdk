# Ginkgo Plugin SDK

**Repository:** https://github.com/usual2970/ginkgo-plugin-sdk

Public SwiftPM package for **Ginkgo external plugins** (interface v1).
Third-party authors depend on this repository — not on the private Ginkgo
app tree. The `plugin-sdk/` folder in the Ginkgo workspace is the same
tree; push changes here to that remote.

| What | Where |
| --- | --- |
| Interface reference | [docs/external-plugins.md](docs/external-plugins.md) |
| 开发 / 安装 / 使用（中文合订） | 工作区 `docs/guides/ginkgo-plugins-guide.md`（发布 SDK 时可复制为 `docs/guide-zh.md`） |
| C header | `Sources/GinkgoPluginABI/include/ginkgo_plugin.h` |
| SwiftPM product | `GinkgoPluginABI` (`import GinkgoPluginABI`) |

## Quick start (Swift)

1. Create a macOS 14+ Swift package with one **dynamic** library product
   per plugin (see the private `ginkgo-plugins` samples for layout).
2. Add the dependency:

```swift
dependencies: [
    .package(url: "https://github.com/usual2970/ginkgo-plugin-sdk.git", from: "1.0.0"),
],
targets: [
    .target(
        name: "MyPlugin",
        // SwiftPM identity from the dependency URL is `ginkgo-plugin-sdk`.
        dependencies: [.product(name: "GinkgoPluginABI", package: "ginkgo-plugin-sdk")],
        path: "Sources/MyPlugin"
    ),
]
```

3. Implement `ginkgo_plugin_entry_v1`, ship a `.ginkgoplugin` bundle
   (`Contents/MacOS`, `Contents/Resources/manifest.json`, ad-hoc or
   Developer ID sign).
4. Users copy the bundle into
   `~/Library/Application Support/Ginkgo/Plugins/` and press **Rescan**
   in Settings → Plugins.

## Versioning

- **`GINKGO_PLUGIN_INTERFACE_VERSION`** in the header (currently `1`)
  must match `interfaceVersion` in your manifest.
- Git **tags** on this repo use semver aligned with that interface:
  `1.0.0` = interface v1. Bump the major tag when the host ships a
  breaking interface v2.

## Maintainer sync

The Ginkgo app embeds the same header under `ginkgo/GinkgoPluginABI/`.
When the contract changes, update both trees and run from the workspace
root:

```bash
./script/verify-plugin-abi-sync.sh
```

## 中文

第三方插件作者只需依赖本仓库（公开），无需 clone 私有 `ginkgo` 应用。
完整契约见 [docs/external-plugins.md](docs/external-plugins.md)；用户安装
方式是把 `.ginkgoplugin` 文件夹拷入 Application Support 下的 Plugins
目录，在设置里 Rescan 并授权 manifest 声明的能力。
