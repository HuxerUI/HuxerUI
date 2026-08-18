// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "@PROJECT_NAME@",
    platforms: [
        .iOS(.v13),
    ],
    products: [
        .library(
            name: "@MODULE_PRODUCT_NAME@",
            targets: ["@MODULE_PRODUCT_NAME@"]
        ),
    ],
    targets: [
        .target(name: "@MODULE_PRODUCT_NAME@"),
    ]
)
