#include "linux_internal.h"
#include "runtime_test_support.h"

#include <gtk/gtk.h>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <huxerui/linux/platform_registry.h>

namespace huxerui::test {
namespace {

constexpr char entry_type[] = "test/GtkEntry";

struct Changed : Event<void(int)> {
  static constexpr char Name[] = "changed";
};

struct Entry {
  Entry() : widget(GTK_WIDGET(g_object_ref_sink(gtk_entry_new()))) {}

  ~Entry() {
    g_object_unref(widget);
  }

  GtkWidget* widget;
};

struct Fixture {
  std::optional<WindowHandle> window;
  State<int> value;
  State<bool> show;
  ScrollController scroll;
  GtkWidget* entry = nullptr;
  PlatformEventEmitter events;
  int creates = 0;
  int updates = 0;
  int disposals = 0;
  int received = 0;
  std::exception_ptr error;
  guint callback = 0;
  int phase = 0;
  gint64 deadline = 0;
};

Fixture* fixture = nullptr;

View App() {
  fixture->window = UseWindow();
  fixture->value = UseState(12345);
  fixture->show = UseState(true);
  View content;
  if (fixture->show.Get()) {
    content = Stack {
      PlatformView(entry_type, fixture->value.Get())
          .On<Changed>([](int value) { fixture->received = value; })
          .With(Frame{.width = 240.0F, .height = 40.0F}),
      Button("Cover").OnClick([] {}).With(Frame{.width = 80.0F, .height = 40.0F}),
    }.With(Frame{.width = 240.0F, .height = 40.0F}, Align(HorizontalAlignment::End, VerticalAlignment::Center));
  }
  return Column {
    Spacer().With(Frame{.height = 24.0F}),
    ScrollView {
      Column {
        Spacer().With(Frame{.height = 30.0F}),
        std::move(content),
        Spacer().With(Frame{.height = 150.0F}),
      }.With(Spacing(0.0F), CrossAlign(CrossAxisAlignment::Start)),
    }.Controller(fixture->scroll).With(Frame{.width = 240.0F, .height = 80.0F}),
  }.With(Spacing(0.0F), CrossAlign(CrossAxisAlignment::Start));
}

void Install(ApplicationContext& root) {
  root.RegisterPlatformView<int>(entry_type, linux::PlatformViewFactory<int, Entry>{
      .create = [](GtkWindow* window, const int& value, PlatformEventEmitter events) {
        REQUIRE(window == linux::GetPlatformWindow(*fixture->window));
        auto entry = std::make_shared<Entry>();
        fixture->entry = entry->widget;
        gtk_editable_set_text(GTK_EDITABLE(entry->widget), std::to_string(value).c_str());
        fixture->events = std::move(events);
        ++fixture->creates;
        return entry;
      },
      .view = [](const std::shared_ptr<Entry>& entry) { return entry->widget; },
      .update = [](Entry& entry, const int& value) {
        gtk_editable_set_text(GTK_EDITABLE(entry.widget), std::to_string(value).c_str());
        ++fixture->updates;
      },
      .dispose = [](Entry& entry) {
        CHECK(gtk_widget_get_parent(entry.widget) == nullptr);
        fixture->entry = nullptr;
        ++fixture->disposals;
      },
  });
}

bool IsEntry(GtkWidget* picked, GtkWidget* entry) {
  return picked && (picked == entry || gtk_widget_is_ancestor(picked, entry));
}

} // namespace

TEST_CASE("Linux PlatformView retains and clips an interactive GTK child") {
  if (gdk_display_get_default() == nullptr) {
    SKIP("GTK display unavailable");
  }

  Fixture test;
  fixture = &test;
  const auto application = Application(App, {
      .window = {.title = "Linux PlatformView test", .initial_size = {280.0F, 120.0F}},
      .application_hooks = {Install, [](ApplicationContext&) {
        fixture->deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        fixture->callback = g_timeout_add(10, +[](gpointer data) -> gboolean {
          auto& test = *static_cast<Fixture*>(data);
          auto& phase = test.phase;
          try {
            CAPTURE(phase);
            REQUIRE(g_get_monotonic_time() < test.deadline);
            GtkWindow* window = test.window ? linux::GetPlatformWindow(*test.window) : nullptr;
            if (phase == 0 && (!window || !test.entry || !gtk_widget_get_mapped(test.entry) ||
                               gtk_widget_get_width(test.entry) == 0)) {
              return G_SOURCE_CONTINUE;
            }
            GtkWidget* canvas = gtk_window_get_child(window);
            if (phase == 0) {
              REQUIRE(test.creates == 1);
              graphene_rect_t bounds;
              REQUIRE(gtk_widget_compute_bounds(test.entry, canvas, &bounds));
              const auto& origin = bounds.origin;
              REQUIRE(IsEntry(gtk_widget_pick(canvas, origin.x + 30, origin.y + 15, GTK_PICK_DEFAULT), test.entry));
              REQUIRE_FALSE(
                  IsEntry(gtk_widget_pick(canvas, origin.x + 210, origin.y + 15, GTK_PICK_DEFAULT), test.entry));
              test.events.Emit<Changed>(7);
              REQUIRE(test.received == 7);
              gtk_editable_select_region(GTK_EDITABLE(test.entry), 1, 4);
              REQUIRE(test.scroll.ScrollTo(50.0F));
            } else if (phase == 1) {
              GtkWidget* clip = gtk_widget_get_parent(test.entry);
              if (gtk_widget_get_height(clip) > 20) {
                return G_SOURCE_CONTINUE;
              }
              graphene_rect_t bounds;
              graphene_rect_t clip_bounds;
              REQUIRE(gtk_widget_compute_bounds(test.entry, canvas, &bounds));
              REQUIRE(gtk_widget_compute_bounds(clip, canvas, &clip_bounds));
              REQUIRE(bounds.size.height >= 40.0F);
              REQUIRE(clip_bounds.origin.y == Catch::Approx(bounds.origin.y + 20.0F));
              const double x = bounds.origin.x + 30.0F;
              REQUIRE(IsEntry(gtk_widget_pick(canvas, x, clip_bounds.origin.y + 8.0F, GTK_PICK_DEFAULT), test.entry));
              REQUIRE_FALSE(IsEntry(gtk_widget_pick(canvas, x, clip_bounds.origin.y - 6.0F, GTK_PICK_DEFAULT),
                                    test.entry));
              REQUIRE(test.scroll.ScrollTo(100.0F));
            } else if (phase == 2) {
              if (gtk_widget_get_mapped(test.entry)) {
                return G_SOURCE_CONTINUE;
              }
              REQUIRE(test.creates == 1);
              REQUIRE(test.disposals == 0);
              REQUIRE(test.scroll.ScrollTo(0.0F));
            } else if (phase == 3) {
              if (!gtk_widget_get_mapped(test.entry)) {
                return G_SOURCE_CONTINUE;
              }
              int start = 0;
              int end = 0;
              REQUIRE(gtk_editable_get_selection_bounds(GTK_EDITABLE(test.entry), &start, &end));
              REQUIRE(start == 1);
              REQUIRE(end == 4);
              REQUIRE(test.updates == 0);
              REQUIRE(gtk_widget_grab_focus(test.entry));
              REQUIRE(IsEntry(gtk_window_get_focus(window), test.entry));
              test.value = 678;
            } else if (phase == 4) {
              if (test.updates == 0) {
                return G_SOURCE_CONTINUE;
              }
              REQUIRE(std::string(gtk_editable_get_text(GTK_EDITABLE(test.entry))) == "678");
              test.show = false;
            } else {
              if (test.disposals == 0) {
                return G_SOURCE_CONTINUE;
              }
              REQUIRE(gtk_window_get_focus(window) != nullptr);
              test.events.Emit<Changed>(9);
              REQUIRE(test.received == 7);
              test.callback = 0;
              UseApplication().Quit();
              return G_SOURCE_REMOVE;
            }
            ++phase;
            return G_SOURCE_CONTINUE;
          } catch (...) {
            test.error = std::current_exception();
            test.callback = 0;
            UseApplication().Quit();
            return G_SOURCE_REMOVE;
          }
        }, fixture);
      }},
  });
  int result = 0;
  try {
    result = detail::RunPlatformApplication(application);
  } catch (...) {
    if (!test.error) {
      test.error = std::current_exception();
    }
  }
  if (test.callback) {
    g_source_remove(test.callback);
  }
  fixture = nullptr;
  REQUIRE(result == 0);
  if (test.error) {
    std::rethrow_exception(test.error);
  }
  REQUIRE(test.creates == 1);
  REQUIRE(test.disposals == 1);
}

} // namespace huxerui::test
