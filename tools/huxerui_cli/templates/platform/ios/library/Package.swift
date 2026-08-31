// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "@PROJECT_NAME@",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "@LIBRARY_PRODUCT_NAME@",
            targets: ["@LIBRARY_PRODUCT_NAME@"]
        ),
    ],
    targets: [
        .target(name: "@LIBRARY_PRODUCT_NAME@"),
    ]
)
