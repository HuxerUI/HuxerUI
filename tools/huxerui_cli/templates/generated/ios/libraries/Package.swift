// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "HuxerUILibraries",
    platforms: [
        .iOS(.v13),
    ],
    products: [
        .library(
            name: "HuxerUILibraries",
            targets: ["HuxerUILibraries"]
        ),
    ],
    dependencies: [
@PACKAGE_DEPENDENCIES@    ],
    targets: [
        .target(
            name: "HuxerUILibraries",
            dependencies: [
@PRODUCT_DEPENDENCIES@            ]
        ),
    ]
)
