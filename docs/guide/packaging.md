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
Linux requires `appimagetool`, `patchelf`, and binutils on `PATH` for `package`; macOS uses the system `otool`, `install_name_tool`, `lipo`, `codesign`, and `hdiutil`.

## Windows URL scheme registration

For a custom scheme owned by your Win32 application, call `windows::RegisterUrlScheme(scheme, display_name)` from `<huxerui/system.h>` explicitly in entry code before `RunApplication()`. It registers the current executable for the current user without administrator privileges, an installer, or CMake registration metadata. This also works in the Windows 7 compatibility backend.

```cpp
#if defined(_WIN32)
windows::RegisterUrlScheme("myapp", "My Application");
#endif
```

The bare scheme is case-insensitive ASCII: a letter followed by letters, digits, '+', '-', or '.', with 2 to 255 characters and no colon. Single-letter schemes are reserved for Windows drive interpretation. The display name is non-empty UTF-8 without nulls or line breaks. Invalid parameters throw `std::invalid_argument`; native failures or ownership conflicts throw `std::runtime_error`. Catch and report startup failures rather than continuing as if registration succeeded.

Registration stores `"<current executable>" "%1"` under `HKCU\Software\Classes\<scheme>\shell\open\command`. Repeating it for the same executable may change the display name. A scheme owned by another executable, a non-protocol class, or a machine-wide registration is rejected rather than replaced. Use a separate development scheme when development and installed copies need to coexist. Registration does not override a user's default-app choice or guarantee that every URL opens this executable; it is not an API for taking over standard browser schemes, file associations, or all-user installation.

External URLs still use `ApplicationHandle::StartupActivation()` and `OnActivation()` with `UrlActivation`. Treat route contents as untrusted input and apply application authorization before opening files or performing other actions. Registration does not add route callbacks or change the existing cold-start and running-window activation flow.

Registration persists after exit. Call `windows::UnregisterUrlScheme(scheme)` explicitly before removing or relocating the owning executable, not on ordinary shutdown. No preceding registration in that process is required. Cleanup removes only matching current-user registration, does nothing when absent, and rejects ownership conflicts without modifying them. It does not clear user default-app choices or registrations belonging to other users. All-user installer cleanup is not implied.

`example_application` registers `huxerui-example` in the [shared example entry point](../../examples/main.cpp), not during static initialization. After closing the example, clean up its protocol before moving or deleting the executable:

```powershell
& <build-directory>/bin/example_application.exe --unregister-url-scheme
```

## Windows notification registration

Ordinary Win32 applications can use native local notifications without MSIX, the Windows App SDK, or CMake-generated notification metadata. The application opts in by passing its stable AppUserModelID, UTF-8 display name, and fixed COM activator CLSID to `windows::RegisterLocalNotifications()` from `<huxerui/system.h>` on its entry thread before `RunApplication()`:

```cpp
#include <huxerui/app.h>
#include <huxerui/system.h>

int main() {
  huxerui::windows::RegisterLocalNotifications(
      "com.example.app", "Example App", "{7451F115-EB29-4BDB-8467-BF2DDDE97980}");
  return huxerui::RunApplication();
}
```

Choose your own App ID and CLSID rather than copying the example identity, and keep both stable across launches and releases. The App ID starts with an ASCII letter or digit, contains only ASCII letters, digits, '.', '_', or '-', and is at most 128 characters. The CLSID is a non-null GUID in braced form. Registration validates and copies its arguments; caller-owned strings need not outlive the call. Each process supports one configured notification identity. Call registration on every launch, including notification-triggered cold launches: the host uses the successful registration's in-process configuration for both sending and COM activation.

Registration writes the current user's COM launch command and app identity/display metadata. It manages only the current-user Start menu shortcut named `<app_id>.lnk`, preserving it if the executable, App ID, and CLSID already match. It neither searches other shortcut locations nor modifies them; ordinary installer shortcuts may coexist with this notification shortcut. The generated installer creates ordinary application shortcuts and does not supply notification metadata or register COM. Repeated registration is allowed only for the same executable. Conflicting registrations and machine-owned identities are rejected, not overwritten. Invalid arguments throw `std::invalid_argument`, switching an already configured process to another identity throws `std::logic_error`, and native failures throw `std::runtime_error`; applications should report errors at their entry boundary. Ordinary host construction and authorization queries never register an application implicitly.

The Windows `example_local_notification` enables a notification-specific branch in the [shared example entry point](../../examples/main.cpp), passing an isolated `.development` App ID and a fixed development CLSID. Build and run it directly without installation, administrator privileges, or a registration script. Other applications choose their own separate development and installed identities in code; CMake does not derive or override notification identity from bundle metadata.

Persistent registration survives ordinary process exit so scheduled notifications and later clicks can still launch the application. This is distinct from the live COM class factory, which the host registers for each process and revokes on exit. For explicit cleanup, call `windows::UnregisterLocalNotifications(app_id, activator_clsid)` before `RunApplication()` or after it returns, never while the notification host is running. No preceding registration call is required. It cancels schedules, clears history, and removes matching current-user registration and the framework-created shortcut path; it also clears a matching in-process identity. Installer-owned shortcuts, other executables, other users, application files, and application data are preserved. Cleanup is not transactional; report errors rather than assuming every native operation completed. The current machine-wide installer does not clean these per-user registrations for all users; applications own explicit per-user cleanup before removal or relocation. Automatic all-user uninstall cleanup is not implemented.

Close the notification example before moving or deleting its executable, then run:

```powershell
& <build-directory>/bin/example_local_notification.exe --unregister-notifications
```

The cleanup argument bypasses registration and application startup. It does not affect a formal installation or silently redirect a registration to another build path.

`RequestAuthorizationAsync()` reports the current Windows notification setting; it does not display a permission prompt. Default notifications and configured XML templates support immediate presentation, system-owned one-shot scheduling, cancellation, and identifier/data round-trip. Windows limits the complete UTF-8 notification XML to 5 KiB, including Base64 data and text, so keep data small even though the shared encoded-data limit is 64 KiB. An oversized request returns `Failed` without replacing existing content. Treat returned activation data as untrusted routing input, not proof of file access or authorization.

Before an application's first native notification, authorization may be `NotDetermined`; this does not prevent submission on Windows. Authorization queries never initialize notification state. The first immediate request sends its real content. A first scheduled request performs framework-owned initialization using a silent, non-popup notification that is immediately removed and expires after fifteen seconds if cleanup fails; a brief notification-center entry is possible, but no banner is requested. Applications must not send their own initialization notifications or interpret `NotDetermined` as an unconditional denial across platforms.

### Windows notification templates

Pass an optional `windows::LocalNotificationTemplateProvider` as the fourth argument to `RegisterLocalNotifications()`. The callback receives `(template_identifier, title, body, data)` as borrowed `std::string_view` values and `const PlatformPayload&`; title and body are already localized UTF-8. It returns `std::optional<std::string>` containing a complete UTF-8 `<toast>` document, or `std::nullopt` for an unknown template. Submit the matching identifier through the ordinary `TemplateNotificationPresentation`; keep XML out of shared application notification data.

```cpp
windows::RegisterLocalNotifications(app_id, display_name, activator_clsid, BuildNotificationTemplate);
```

An empty provider disables `can_use_templates`; a non-empty provider enables the mechanism without promising every identifier. Default presentation bypasses it. Successful registration replaces the previous provider, including clearing it when omitted; failed registration preserves the old configuration. Configure it before host startup, never while the host is running. The process retains the callback and each host takes a copy, so capture durable application-shell values rather than Runtime, View, composition state, or borrowed references. Explicit unregistration also clears the matching process provider.

The provider runs synchronously on the host UI thread at submission time, including for `ScheduleAsync()`. Do not block on I/O or retain parameter references. The completed XML is submitted to Windows immediately; scheduled delivery and later clicks do not call the provider. Images referenced by the XML must remain accessible through native delivery, even after the submitting process exits.

Return one `<visual>` containing one `<binding template="ToastGeneric">` under `<toast>`. Native text, images, groups, and progress elements remain application-owned; use an XML DOM or properly escape all dynamic text and attribute values. HuxerUI rejects DTDs, external entities, `launch`, `activationType`, and `protocolActivationTargetApplicationPfn` on the root, and any `actions`, `input`, or `header` elements. These routes need separate activation contracts, not just XML support. HuxerUI injects its own `launch` envelope and retains native tag/group, primary-click data, scheduling, replacement, and cancellation. It checks the final encoded size before touching existing notifications; empty or malformed XML, forbidden routing, provider exceptions, and excessive size return `Failed`, while unknown templates return `Unavailable` without fallback. Windows remains responsible for its full XML schema, feature availability, and final presentation; successful parsing does not guarantee visibility.

The [Windows download template](../../examples/local_notification/windows/notification_template.cpp) demonstrates DOM-based text escaping and locale-independent progress formatting. Progress bars require Windows 10 version 1703 or later. The example's detailed-layout switch adds text and byte counts rather than controlling system expansion. Repeated `ShowAsync()` calls replace notifications and can show another popup even with silent audio; they do not use Windows data-binding updates. Native in-place updates and custom action/input handling are outside the current API.

## Application payloads

CMake install rules are the only source of application files placed into desktop packages.
The generated platform shell installs the application executable or bundle and its final HuxerUI resource package, then collects non-system dynamic dependencies recursively during `cmake --install`.
Deployment scans the original binaries, copies dependencies, fixes loading paths in the staged copies, and verifies that the staged application no longer resolves non-system dependencies outside the package.
Each package operation uses a fresh staging directory.

Libraries loaded through `LoadLibrary`, `dlopen`, or an equivalent runtime mechanism may not appear in the binary dependency graph.
Declare those additional roots with `huxerui_add_runtime_dependencies`; the helper installs them and collects their dependencies:

```cmake
huxerui_add_runtime_dependencies(my_app
        TARGETS image_codec
        FILES "${vendor_runtime_file}"
        SEARCH_DIRECTORIES "${vendor_runtime_directory}"
)
```

`TARGETS` accepts shared-library and module targets, including imported targets; application builds also build any declared local targets.
`FILES` accepts prebuilt dynamic libraries and, on macOS, complete `.framework` or `.bundle` directories.
Relative file and search paths are resolved against the declaring source directory, and generator expressions select the active build configuration.
Conditional file and search expressions that evaluate to an empty value are skipped, and imported targets retain the visibility of their declaring directory.
Search hints include the application directory, declared targets, and directly discoverable linked CMake targets; provide `SEARCH_DIRECTORIES` for other locations, including target paths hidden behind conditional link expressions.
Search directories are packaging inputs, not paths retained by the installed application.

The optional `DESTINATION` selects a subdirectory relative to the Windows application directory, macOS bundle's `Contents`, or Linux AppDir's `usr` directory.
Without it, Windows libraries go beside the executable, macOS libraries and frameworks go into `Frameworks` (bundles into `PlugIns`), and Linux libraries go into `lib`.
Dependencies of explicitly placed modules still use the platform's default library directory.
The application remains responsible for locating and loading its explicitly declared modules.

Install data and configuration with the existing application component:

```cmake
get_target_property(app_install_component my_app HUXERUI_APPLICATION_INSTALL_COMPONENT)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/codec-settings.json"
        DESTINATION config
        COMPONENT "${app_install_component}"
)
```

Use an appropriate destination and platform condition for each target system.
Missing required dependencies, conflicting payloads, architecture mismatches, broken links, loading-path errors, and invalid final signatures fail packaging before `dist` is replaced.
Native binaries installed with application-owned rules also participate in the final dependency validation.
System runtimes cannot be explicitly bundled through the helper or application-owned installation rules.
The CLI wraps the validated installation tree; CMake owns dependency deployment.

## Application icons

Application icons belong to the generated platform shell and use each platform's native format.
New applications start with the HuxerUI brand mark as an editable placeholder.
Replace the generated file in place; no CMake option or HuxerUI resource registration is required:

- Windows uses `platform/windows/app.ico` for the application executable, installer executable, Burn setup, Start menu and desktop shortcuts, and the installed-app entry.
- macOS uses `platform/macos/AppIcon.icns` for the application bundle displayed by Finder, Dock, and the application switcher.
- Linux uses `platform/linux/package/<target>.svg` for the AppImage and its desktop entry.
- Android uses the legacy density icons and adaptive icon resources under `platform/android/app/src/main/res`.
- iOS uses the complete `platform/ios/App/Assets.xcassets/AppIcon.appiconset` catalog.
- Web uses `platform/web/favicon.svg` for browser tabs and `apple-touch-icon.png` for home-screen bookmarks.

The Windows installer reuses the application icon, and the macOS DMG retains its standard volume icon.
HuxerUI does not convert a common source image into platform icon formats during application builds.

## Desktop system requirements

Windows deployment targets the default Windows 10 version 1607 or later platform and keeps Windows system DLLs, API sets, and UCRT system-owned.
System DLLs are identified by the deployment module's explicit Windows library list; a third-party DLL is not excluded merely because it resides in `System32` or `SysWOW64`.
The optional Windows 7 backend configuration does not have a packaging redistribution policy and is rejected by `package`.
Required Release VC++ runtime DLLs are deployed beside both the application and the independently staged installer interface, so target machines do not need a separate VC++ Redistributable installation.
The runtime files must come from the matching toolchain's redistributable directories; missing files, older runtime versions, and Debug CRT dependencies fail packaging.
HuxerUI uses CMake's detected `MSVC_REDIST_DIR` and falls back to the active Visual Studio compiler or developer environment when CMake does not yet recognize a newer toolset.
Set `MSVC_REDIST_DIR` explicitly when the toolchain stores its redistributables in a separate location; an explicit directory remains authoritative and an invalid one fails packaging.
Application publishers own updates to these app-local runtime files.
WiX and its .NET runtime are build-machine tools, not installed-application prerequisites.

macOS keeps Apple system frameworks and `/usr/lib` libraries system-owned, including libraries supplied through the dyld shared cache.
Non-system dylibs and frameworks are copied into the application bundle with their runtime resources and symbolic links.
Deployment rewrites load commands and runpaths to use bundle-relative locations, checks required architectures and declared minimum macOS versions, and applies ad-hoc signatures from nested code outward after modification.
Developer ID signing and notarization belong to the application's release pipeline and must follow dependency deployment; the default ad-hoc signature does not provide Gatekeeper distribution approval.

Linux keeps glibc, the ELF loader, the system C++ runtime, graphics drivers, and the distribution-provided GTK 4.14+, libepoxy, Pango, Cairo, GIO, and libsoup 3 runtime stack system-owned.
The desktop stack's resolved distribution-library dependency closure is excluded; other application libraries are bundled even when installed in a system search directory.
Custom desktop-stack builds outside distribution library directories are rejected.
The target machine needs the corresponding runtime packages, not their development packages or the HuxerUI SDK.
Bundled ELF files use `$ORIGIN`-relative RUNPATH entries, and versioned library link chains are preserved.
Deployment prints the packaged binaries' `GLIBC`, `GLIBCXX`, and `CXXABI` requirements; build and test release packages on the intended minimum environment.
The official SDK's glibc 2.38 requirement alone does not establish an application's minimum system requirements or provide GTK through the AppImage.

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
