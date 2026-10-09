// swift-tools-version:5.9
//
// Public author SDK for Ginkgo external plugins (interface v1). The host app
// ships the same header in its private tree; keep both in sync with
// ginkgo-workspace/script/verify-plugin-abi-sync.sh.

import PackageDescription

let package = Package(
    name: "GinkgoPluginSDK",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "GinkgoPluginABI", targets: ["GinkgoPluginABI"]),
    ],
    targets: [
        .target(
            name: "GinkgoPluginABI",
            path: "Sources/GinkgoPluginABI"
        ),
    ]
)
