#include <huxerui/huxerui.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace huxerui;

struct TabGroup {
  std::vector<std::string> tabs;
  std::string selected;
};

struct WorkspaceTabs {
  TabGroup main{{"Plan", "Notes"}, "Plan"};
  TabGroup secondary{{"Draft", "Review"}, "Draft"};
};

struct TabTransfer {
  std::string tab;
  bool from_secondary;
};

[[huxerui::composable]]
View TabPanel(State<WorkspaceTabs> workspace, bool secondary) {
  const auto& group = secondary ? workspace->secondary : workspace->main;
  const auto& colors = UseTheme().colors;
  return Column {
    Row {
      ForEach(group.tabs, [&](const std::string& tab) {
        const bool selected = tab == group.selected;
        auto preview = [tab, background = colors.primary_container, foreground = colors.on_primary_container] {
          return Text(tab).With(Padding(8.0F), Background{background}, Foreground(foreground), CornerRadius(6.0F));
        };
        return Chip(tab, selected)
            .With(
                Semantics{.role = SemanticRole::Tab},
                DragSource(TabTransfer{tab, secondary}, preview).Scope(DragDropScope::Application)
            )
            .OnChanged([workspace, secondary, tab](bool select) {
              if (!select) return;
              workspace.Update([&](WorkspaceTabs& next) {
                (secondary ? next.secondary : next.main).selected = tab;
              });
            })
            .Key(tab);
      }),
      group.tabs.empty() ? Text("Drop tabs here") : View{},
    }.With(
        Frame{.min_height = 52.0F},
        Padding(8.0F),
        Spacing(6.0F),
        CrossAlign(CrossAxisAlignment::Center),
        Background{colors.surface_container_highest},
        CornerRadius(8.0F),
        Semantics{.role = SemanticRole::TabList},
        DropTarget::Accepts<TabTransfer>([secondary](const TabTransfer& transfer) {
          return transfer.from_secondary != secondary;
        }).Scope(DragDropScope::Application)
    )
        .On<DropEvents<TabTransfer>::Dropped>([workspace, secondary](const TabTransfer& transfer, const DropEvent&) {
          workspace.Update([&](WorkspaceTabs& next) {
            auto& source = secondary ? next.main : next.secondary;
            auto& target = secondary ? next.secondary : next.main;
            const auto tab = std::ranges::find(source.tabs, transfer.tab);
            if (tab == source.tabs.end()) return;

            target.tabs.push_back(transfer.tab);
            target.selected = transfer.tab;
            const auto following = source.tabs.erase(tab);
            if (source.selected == transfer.tab) {
              if (source.tabs.empty()) {
                source.selected.clear();
              } else {
                source.selected = following == source.tabs.end() ? source.tabs.back() : *following;
              }
            }
          });
        }),
    group.selected.empty() ? View{} : Text("Viewing " + group.selected, TextRole::Title),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View App() {
  auto application = UseApplication();
  auto workspace = UseState(WorkspaceTabs{});
  auto secondary = UseState(std::optional<WindowHandle>{});
  const std::optional<WindowHandle>& secondary_window = secondary;
  const bool open = secondary_window && secondary_window->IsOpen();
  return Column {
    Button(open ? "Activate secondary" : "Open secondary").OnClick([application, workspace, secondary] {
      const std::optional<WindowHandle>& current = secondary;
      if (current && current->IsOpen()) {
        current->Activate();
      } else {
        secondary = application.OpenWindow([workspace] {
          return Column {
            TabPanel(workspace, true),
          }.With(Padding(24.0F), CrossAlign(CrossAxisAlignment::Stretch));
        }, {
            .title = "Secondary window",
            .initial_size = {480.0F, 300.0F},
        });
      }
    }).With(Enabled(application.SupportsMultipleWindows())),
    Text("Drag a tab onto the tab bar in the other window."),
    TabPanel(workspace, false),
  }.With(Padding(24.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

const Application application{
    App,
    {
        .window = {.title = "HuxerUI Multiple Windows", .initial_size = {480.0F, 340.0F}},
    },
};
