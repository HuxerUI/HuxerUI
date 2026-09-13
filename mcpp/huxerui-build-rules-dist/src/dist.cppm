// The distribution formats of a HuxerUI application built by mcpp -- msi,
// appimage, app, web, apk -- as `mcpp:plugins`' dist members configured the
// HuxerUI way. A host module the framework re-exports; `huxerui.rules`
// composes these options into its own and calls provide_formats() for an
// application.

export module huxerui.rules.dist;

import std;
import mcpp;
import mcpp.dist.wix;
import mcpp.dist.appimage;
import mcpp.dist.apple;
import mcpp.dist.web;
import mcpp.dist.apk;

export namespace huxerui::rules {

// What an application must state to get a Windows installer, and nothing it
// could have been asked for twice. The format itself is `mcpp:plugins`'
// `dist-wix`; this is the HuxerUI-side spelling of its options, so an
// application's build program reads like huxerui_add_app() in
// cmake/HuxerUIApp.cmake. Every field is optional: the member derives the
// version from [package], the manufacturer from the authors or the namespace,
// and the upgrade code deterministically from the package identity -- a
// stable GUID chosen once per product, which is exactly what a derived value
// is. The format is provided on every Windows build; these only change it.
struct installer_options {
    std::string target;         // the [targets.*] app to install; default: options::target
    std::string version;        // default: [package] version, made MSI-shaped
    std::string upgrade_code;   // default: derived from namespace + name
    std::string manufacturer;   // default: the first author, else the namespace
    std::string display_name;   // default: the target name
};

// What an application may state about its AppImage; `dist-appimage` does the
// rest, with a placeholder icon when none is named. The Windows counterpart
// is installer_options above.
struct appimage_options {
    std::string target;                  // the [targets.*] app to package; default: options::target
    std::string display_name;            // default: the target name
    std::string icon;                    // a .png, relative to the manifest
    std::vector<std::string> categories; // freedesktop categories; default "Utility"
};

// What an application may state about its `.app` bundle on macOS and iOS;
// `dist-apple` does the rest. On macOS `icon` is one `.icns` file; on iOS it
// is a DIRECTORY of flat PNGs, which the member lists under CFBundleIcons.
struct apple_options {
    std::string target;         // the [targets.*] app to bundle; default: options::target
    std::string display_name;   // CFBundleName; default: the target name
    std::string bundle_id;      // CFBundleIdentifier; default: derived from namespace + name
    std::string icon;           // .icns (macOS) or a directory of .png (iOS), relative to the manifest
};

// The page `mcpp pack --format web` writes beside the launcher. Every HuxerUI
// application is a Web application when built for wasm32-emscripten, so the
// format is always provided; these only replace the page the rule ships.
struct web_options {
    std::string template_file;  // package-root-relative; `{{name}}` and `{{title}}` are substituted
    std::string title;          // <title>; default: the package name
};

// What an application may state about its APK; every field has a default that
// `dist-apk` or this rule derives, so a template application states nothing.
struct android_options {
    std::string application_id;     // manifest package; default: <namespace>.<name>
    std::string label;              // android:label; default: the package name
    std::string activity;           // the launcher Activity; default: org.huxerui.HuxerUIActivity
    std::string java;               // a directory of the application's own Java, relative to the manifest; default: android/java when present
    std::string res;                // an aapt2 `res/` directory, relative to the manifest; default: android/res when present, else the SDK's launcher icon set
    std::string manifest_template;  // replaces the manifest the rule ships, relative to the manifest
};

// What provide_formats() is given: the application's target and the five
// option sets `huxerui::rules::options` carries under the same names.
struct formats {
    std::string       target;
    installer_options installer;
    appimage_options  appimage;
    apple_options     apple;
    web_options       web;
    android_options   android;
};

// ------------------------------------------------------------ dist members --
// A member's plan says why it does not apply, on stderr -- which mcpp
// discards when the build program succeeds. When the format the member
// provides is the one `mcpp pack` asked for, that reason is the whole
// diagnosis, so it is repeated through `mcpp::warning`, the channel that is
// shown; on every other build the member is quiet and so is this.
template <class Plan>
bool run_member(const char* format, const Plan& plan, bool (*submit)(const Plan&)) {
    if (!plan.applies && std::string_view(mcpp::pack_format()) == format) {
        std::string message = std::string("huxerui.rules: dist-") + format + " declined this build";
        if (!plan.reason.empty()) message += ": " + plan.reason;
        mcpp::warning(message.c_str());
    }
    return submit(plan);
}

// Every format the row serves, provided for `mcpp pack --format <name>` with
// nothing stated; the options only change what a format produces. `root` is
// the HuxerUI SDK root, where the page template, the Android manifest and the
// launcher icon set the SDK ships live.
inline bool provide_formats(const formats& opt, const std::string& root) {
    // The distribution formats are `mcpp:plugins`' dist members, reached
    // through this package's own build-dependency. Each is provided on the
    // row it serves and nowhere else, so `mcpp pack --format apk` on a Linux
    // desktop build is an unknown format rather than a declined one; a
    // member's plan is a no-op until `mcpp pack` names its format, so this
    // costs an ordinary build nothing. The payloads they run (xim:wix,
    // xim:appimagetool, the NDK's build tools, ...) are declared by the
    // members themselves, and a host module's declaration reaches every
    // build program it is compiled into -- which is why an application
    // declares none of them.
    const std::string dist_os  = mcpp::target_os();
    const std::string dist_env = mcpp::target_env();
    const auto target_or = [&](const std::string& named) { return named.empty() ? opt.target : named; };
    if (dist_os == "windows") {
        mcpp::dist::wix::options w;
        w.target       = target_or(opt.installer.target);
        w.product_name = opt.installer.display_name;
        w.manufacturer = opt.installer.manufacturer;
        w.version      = opt.installer.version;
        w.upgrade_code = opt.installer.upgrade_code;
        mcpp::provides_pack_format("msi");
        if (!run_member("msi", mcpp::dist::wix::plan_for(w), &mcpp::dist::wix::submit)) return false;
    }
    if (dist_os == "linux" && dist_env != "android") {
        mcpp::dist::appimage::options a;
        a.target     = target_or(opt.appimage.target);
        a.app_name   = opt.appimage.display_name;
        a.icon       = opt.appimage.icon;
        a.categories = opt.appimage.categories;
        a.terminal   = false;
        mcpp::provides_pack_format("appimage");
        if (!run_member("appimage", mcpp::dist::appimage::plan_for(a), &mcpp::dist::appimage::submit)) return false;
    }
    if (dist_os == "macos" || dist_os == "ios") {
        mcpp::dist::apple::options a;
        a.target    = target_or(opt.apple.target);
        a.app_name  = opt.apple.display_name;
        a.bundle_id = opt.apple.bundle_id;
        a.icon      = opt.apple.icon;
        mcpp::provides_pack_format("app");
        if (!run_member("app", mcpp::dist::apple::plan_for(a), &mcpp::dist::apple::submit)) return false;
    }
    if (dist_os == "emscripten") {
        // The page the rule ships calls the MODULARIZE factory
        // the emscripten section exports and mounts the application; a
        // project supplies its own to change the page, not the contract.
        // `dist-web` joins the template path to the manifest directory, and
        // an absolute path survives that join, which is how the rule's own
        // template -- in the SDK, not the project -- reaches it.
        mcpp::dist::web::options w;
        w.target        = opt.target;
        w.template_file = opt.web.template_file.empty()
            ? root + "/mcpp/huxerui-build-rules-dist/web/index.html.in" : opt.web.template_file;
        w.title         = opt.web.title;
        mcpp::provides_pack_format("web");
        if (!run_member("web", mcpp::dist::web::plan_for(w), &mcpp::dist::web::submit)) return false;
    }
    if (dist_env == "android") {
        // Level 1 of dist-apk with the framework's Java host as the first
        // root and, when present, the
        // application's own as the second; the Activity is the framework's
        // unless the application names its own subclass.
        const std::string manifest_dir = mcpp::manifest_dir();
        const auto under_manifest = [&](const std::string& rel) {
            return (std::filesystem::path(manifest_dir) / rel).string();
        };
        mcpp::dist::apk::options a;
        a.target         = opt.target;
        a.application_id = opt.android.application_id;
        a.label          = opt.android.label;
        a.activity       = opt.android.activity.empty() ? std::string("org.huxerui.HuxerUIActivity")
                                                        : opt.android.activity;
        a.java_sources   = { root + "/platform/android/huxerui/src/main/java" };
        const std::string java = opt.android.java.empty() ? std::string("android/java") : opt.android.java;
        if (std::filesystem::is_directory(under_manifest(java))) a.java_sources.push_back(under_manifest(java));
        // The application's res/ when it has one, else the launcher icon set
        // the SDK's Gradle template ships -- one icon in the repository, and
        // every APK has one. An application's own res/ replaces it whole, so
        // it carries `@mipmap/ic_launcher`, which the manifest names.
        const std::string res = opt.android.res.empty() ? std::string("android/res") : opt.android.res;
        a.resources = std::filesystem::is_directory(under_manifest(res))
            ? under_manifest(res)
            : root + "/tools/huxerui_cli/templates/platform/android/app/app/src/main/res";
        // The framework's Java root is a dependency's, so its rerun glob would
        // match nothing; the list the rule package carries is what re-runs
        // this program when a host file is added or removed.
        mcpp::rerun_if_changed((root + "/mcpp/huxerui-build-rules-dist/android/java-sources.txt").c_str());
        // dist-apk joins the template to the manifest directory; an absolute
        // path survives the join, which is how the SDK's own reaches it.
        a.manifest_template = opt.android.manifest_template.empty()
            ? root + "/mcpp/huxerui-build-rules-dist/android/AndroidManifest.xml.in"
            : under_manifest(opt.android.manifest_template);
        mcpp::provides_pack_format("apk");
        if (!run_member("apk", mcpp::dist::apk::plan_for(a), &mcpp::dist::apk::submit)) return false;
    }
    return true;
}

} // namespace huxerui::rules
