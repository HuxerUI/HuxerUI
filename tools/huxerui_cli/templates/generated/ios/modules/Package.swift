// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "HuxerUIModules",
    platforms: [
        .iOS(.v13),
    ],
    products: [
        .library(
            name: "HuxerUIModules",
            targets: ["HuxerUIModules"]
        ),
    ],
    dependencies: [
@PACKAGE_DEPENDENCIES@    ],
    targets: [
        .target(
            name: "HuxerUIModules",
            dependencies: [
@PRODUCT_DEPENDENCIES@            ]
        ),
    ]
)
