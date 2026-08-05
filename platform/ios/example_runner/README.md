# iOS Example Runner

Open `HuxerUIExamples.xcodeproj` to run and debug repository examples on a Simulator or physical device.

The default application target is `example_ui_gallery`. To select another example, copy `Config/Local.xcconfig.example` to the ignored `Config/Local.xcconfig` and set `HUXERUI_APP_TARGET` to an `example_*` target declared under `examples/`.

Set `DEVELOPMENT_TEAM` in the same local file when physical-device signing requires it. The Xcode build phase configures the repository for the selected SDK and architecture, builds only the selected application core, and links it into the shared native runner.
