# Packaging Applications

`huxerui package` builds an enabled platform and publishes its distributable output under `dist/<platform>`.
Release is the default profile for packaging; an explicit `--profile` selects another configured profile.

```bash
huxerui package windows
huxerui package macos,linux --profile release
```

Packaging uses the same application target, resources, libraries, and platform shell as `build` and `run`.
Intermediate files stay under `.huxerui/package`, and the CLI replaces a platform's published directory only after the new artifacts are complete.

Desktop outputs are platform-native:

- Windows produces one self-contained Burn setup executable containing the application MSI and a HuxerUI installer interface.
- macOS produces one DMG containing the application bundle and an Applications link.
- Linux produces one AppImage.
- Android publishes the generated APK, iOS publishes its built application bundle, and Web publishes the generated deployment files.

Normal `build` and `run` do not build installer targets or require packaging tools.
Windows downloads the pinned WiX v5 package dependencies only during `package` and verifies their SHA-256 hashes.
Windows setup generation currently supports x64 applications.
Running the restored WiX tool requires `Microsoft.NETCore.App` 6.0 or newer; HuxerUI reports this package-only prerequisite without requiring a system-wide WiX installation.
Linux requires `appimagetool` on `PATH` for `package`; macOS uses the system `hdiutil`.

## Application payloads

CMake install rules are the only source of application files placed into desktop packages.
The generated platform shell already installs the application executable or bundle and its final HuxerUI resource package.
Declare third-party dynamic libraries, codecs, plugins, and data explicitly with the application install component:

```cmake
get_target_property(app_install_component my_app HUXERUI_APPLICATION_INSTALL_COMPONENT)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/image_codec.dll"
        DESTINATION .
        COMPONENT "${app_install_component}"
)
```

Use an appropriate destination and platform condition for each target system.
The CLI does not scan adjacent build outputs or infer runtime dependencies, so an undeclared file is not silently added to a package.

## Custom Windows installer interface

New Windows application shells contain an editable installer application under `platform/windows/package`:

```text
platform/windows/package/
  Bundle.wxs.in
  Package.wxs.in
  resources/
    strings/
      default.properties
  src/
    app.cpp
    main.cpp
```

Edit `src/app.cpp` and `resources` with ordinary HuxerUI components and assets.
The generated interface resolves its text through the ordinary HuxerUI resource system: `default.properties` is the required fallback, and locale catalogs such as `zh.properties`, `ja.properties`, or `pt-BR.properties` follow the same language-tag selection and fallback rules as application resources.
Windows supplies localized text for the native folder picker, while messages returned by the installation engine remain engine-owned text.
Edit the WiX sources for product metadata or MSI behavior that belongs to the Windows package.
Keep installation mechanics in Burn instead of reimplementing file copying, elevation, rollback, repair, or uninstall in the interface.
The generated interface displays the expanded default installation directory, accepts an absolute path, opens the Windows folder picker from its Browse button, and lets the user choose whether to create a desktop shortcut.
The Start menu shortcut is always installed so the application remains discoverable through the normal Windows application surface.
Taskbar pinning does not belong to setup: an application that supports it must request the operation from its foreground interface and let Windows obtain user confirmation.

The Windows-only `<huxerui/windows/installer.h>` API exposes one root-owned session:

```cpp
#include <huxerui/huxerui.h>
#include <huxerui/windows/installer.h>

using namespace huxerui;
using namespace huxerui::windows;

View InstallerPage() {
  const InstallerHandle installer = UseInstaller();
  const TaskScope tasks = UseTaskScope();
  const InstallerStatus status = installer.Status();
  if (status.phase == InstallerPhase::Ready && status.product == InstallerProductState::Absent) {
    return Button("Choose destination and install").OnClick([installer, status, tasks] {
      tasks.Launch([installer, status]() -> Task<void> {
        const std::optional<std::filesystem::path> selected =
            co_await installer.ChooseDestinationAsync(status.default_destination);
        if (selected) {
          installer.Install({
              .destination = *selected,
              .create_desktop_shortcut = status.default_create_desktop_shortcut,
          });
        }
      });
    });
  }
  return ProgressBar(status.progress);
}

const Application application{
    InstallerPage,
    {.root_hooks = {InstallInstallerSession}},
};
```

`InstallerHandle` starts install, repair, and uninstall operations, requests cooperative cancellation, and answers the current identified prompt.
`InstallerInstallOptions` overrides the authored destination or desktop-shortcut choice for one install request; leaving either field unset preserves the matching Burn variable.
`ChooseDestinationAsync()` opens the Windows folder picker from a Task launched by the component's `TaskScope` and returns no value when the user dismisses it.
`InstallerStatus` is the single observable status value for phase, detected product state, expanded defaults, action, progress, current package, prompt, failure, and restart requirement.
Do not use this API in the ordinary application executable or introduce another installer state store beside it.

Signing, notarization, store submission, and platform-specific release credentials remain application and release-pipeline responsibilities.
