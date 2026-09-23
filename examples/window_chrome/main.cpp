#include <huxerui/huxerui.h>

#include <window_chrome_resources.h>

using namespace huxerui;

struct WindowChromeViewModel {
  std::optional<WindowHandle> window;

  void ShowWindow() const {
    if (window) window->Activate();
  }
};

void InstallWindowChrome(ApplicationContext& context) {
  auto model = std::make_shared<WindowChromeViewModel>();
  context.Provide(model);
  const auto application = UseApplication();
  const auto tray = application.SystemTray();
  tray.OnActivate([model] { model->ShowWindow(); });
  tray.Show(window_chrome::images::tray, {
      .tooltip = "HuxerUI Window Chrome",
      .menu = {
          MenuItem("Show window", [model] { model->ShowWindow(); }),
          MenuSection{},
          MenuItem("Quit", [application] { application.Quit(); }),
      },
  });
}
[[huxerui::composable]]
View WindowChromeDemo() {
  const ThemeSpec& theme = UseTheme();
  const ApplicationHandle application = UseApplication();
  const WindowHandle window = UseWindow();
  const auto model = UseService<WindowChromeViewModel>();
  Lifecycle([model, window] {
    model->window = window;
    return [model] { model->window.reset(); };
  });
  const SystemTrayHandle tray = application.SystemTray();
  const bool tray_available = tray.IsAvailable();
  auto exit_dialog_open = UseState(false);
  auto toast = UseToast();
  auto dialog = UseDialog();

  Lifecycle([window, tray_available] {
    if (!tray_available) {
      window.Show();
    }
  }, tray_available);
  window.OnMinimizeRequest(
      [window, tray_available] {
        if (!tray_available) {
          return false;
        }
        window.Hide();
        return true;
      },
      tray_available
  );
  window.OnCloseRequest(
      [application, dialog, exit_dialog_open, tray_available, window]() mutable {
        if (!tray_available) {
          return false;
        }
        if (exit_dialog_open.Get()) {
          return true;
        }
        exit_dialog_open = true;
        dialog.Show(
            "Exit HuxerUI?",
            "Would you like to exit or keep the application running in the system tray?",
            "Exit",
            "Minimize to tray",
            [application, exit_dialog_open]() mutable {
              exit_dialog_open = false;
              application.Quit();
            },
            [exit_dialog_open, window]() mutable {
              exit_dialog_open = false;
              window.Hide();
            },
            DialogOptions{
                .dismiss_on_outside_press = false,
                .dismiss_on_cancel = false,
            }
        );
        return true;
      },
      tray_available
  );

  return Column {
    WindowTitleBar {
      Text("HuxerUI", TextRole::Label).With(Foreground(theme.colors.on_primary)),
      Text("samples / window_chrome.cpp", TextRole::Label)
          .With(Foreground(theme.colors.on_primary), Grow(1.0F)),
      Button("Hide to tray")
          .OnClick([toast, tray_available, window] {
            if (tray_available) {
              window.Hide();
            } else {
              toast.Show("System tray is unavailable");
            }
          })
          .With(Frame{.height = 30.0F}),
    }.With(
        Padding(EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.extra_small)),
        Spacing(theme.spacing.small),
        Background(theme.colors.primary)
    ),
    Row {
      Column {
        Text("Workspace", TextRole::Title),
        Text("examples", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
        Text("  window_chrome", TextRole::Label),
        Text("    main.cpp", TextRole::Label).With(Foreground(theme.colors.primary)),
        Text("    CMakeLists.txt", TextRole::Label),
        Text("    resources/images/tray.png", TextRole::Label),
        Spacer().With(Grow(1.0F)),
        Text("Custom chrome and the system tray form one application shell.", TextRole::Label)
            .With(Foreground(theme.colors.on_surface_variant)),
      }.With(
          Frame{.width = 220.0F},
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
          Background(theme.colors.surface_container_low)
      ),
      Column {
        Row {
          Text("window_chrome.cpp", TextRole::Label),
          Text("Custom chrome", TextRole::Label)
              .With(Foreground(theme.colors.on_surface_variant), Grow(1.0F)),
        }.With(Spacing(theme.spacing.medium)),
        Divider(),
        Column {
          Text("Application-defined window shell", TextRole::Title),
          Text("Window controls belong to this window; the system tray remains available for the application lifetime."),
          Text(tray_available ? "System tray is available" : "System tray is unavailable", TextRole::Label)
              .With(Foreground(tray_available ? theme.colors.primary : theme.colors.error)),
          Text("Minimize hides the window when the tray is available. Activating the tray icon restores it."),
          Text("Close asks whether to exit or continue running in the tray."),
          Button("Interactive controls remain client content")
              .OnClick([toast] { toast.Show("Client interaction received"); }),
        }.With(Padding(theme.spacing.extra_large), Spacing(theme.spacing.medium)),
      }.With(Grow(1.0F), Background(theme.colors.surface)),
    }.With(Grow(1.0F)),
  }.With(
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme {WindowChromeDemo()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Window Chrome",
            .initial_size = {760.0F, 480.0F},
            .minimum_size = Size{600.0F, 360.0F},
            .chrome_mode = WindowChromeMode::Custom,
            .title_bar_height = 48.0F,
        },
        .show_debug_overlay = false,
        .application_hooks = {InstallWindowChrome},
    }
};
