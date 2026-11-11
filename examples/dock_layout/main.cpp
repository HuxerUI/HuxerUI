#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace huxerui;

namespace {

struct ProjectFile {
  std::string_view path;
  std::string_view language;
  std::string_view badge;
  std::string_view source;

  std::string_view Name() const {
    const auto separator = path.rfind('/');
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
  }
};

constexpr std::array project_files{
    ProjectFile{"src/workspace.cpp", "C++", "C++", R"cpp(#include <huxerui/huxerui.h>
#include "palette.h"

using namespace huxerui;

View Workspace() {
  return Column {
    Text("Harbor", TextRole::Title),
    Text("A space for your next project"),
    Button("New project"),
  }.With(
      Padding(24.0F),
      Spacing(12.0F),
      Background(canvas_color)
  );
}
)cpp"},
    ProjectFile{"src/palette.h", "C++", "C++", R"cpp(#pragma once

#include <huxerui/color.h>

inline constexpr auto canvas_color =
    huxerui::Color::Rgb(239, 245, 252);
inline constexpr auto accent_color =
    huxerui::Color::Rgb(37, 94, 169);
)cpp"},
    ProjectFile{"harbor.toml", "TOML", "CFG", R"toml([project]
name = "Harbor"
entry = "src/workspace.cpp"

[window]
width = 960
height = 640
)toml"},
    ProjectFile{"README.md", "Markdown", "MD", R"md(# Harbor

A small HuxerUI project for sketching workspace layouts.

## Working on this project

- Edit the workspace in src/workspace.cpp.
- Adjust its colors in src/palette.h.
- Keep window defaults in harbor.toml.
)md"},
};
constexpr std::size_t source_folder = project_files.size();

struct Document {
  TextEditingValue editing;
  std::string saved;

  bool Modified() const { return editing.text != saved; }
  bool operator==(const Document&) const = default;
};

using Documents = std::array<Document, project_files.size()>;
using ActivityLog = State<std::vector<std::string>>;

Documents InitialDocuments() {
  Documents documents;
  for (std::size_t index = 0; index < documents.size(); ++index) {
    auto& document = documents[index];
    document.saved = project_files[index].source;
    document.editing = TextEditingValue::FromText(document.saved);
    document.editing.selection = {};
  }
  return documents;
}

void RecordActivity(ActivityLog activity, std::string message) {
  activity.Update([&](auto& entries) {
    if (entries.size() == 40) {
      entries.erase(entries.begin());
    }
    entries.push_back(std::move(message));
  });
}

DockTree InitialLayout(bool compact) {
  if (compact) {
    return DockTree::Tabs({"editor", "project", "inspector", "activity"});
  }
  return DockTree::Split(Axis::Horizontal, {
      DockTree::Tabs({"project"}).With(Frame{.width = 208.0F, .min_width = 156.0F}),
      DockTree::Split(Axis::Vertical, {
          DockTree::Tabs({"editor"}).With(Grow(), Frame{.min_height = 190.0F}),
          DockTree::Tabs({"activity"}).With(Frame{.height = 166.0F, .min_height = 112.0F}),
      }).With(Grow(), Frame{.min_width = 264.0F}),
      DockTree::Tabs({"inspector"}).With(Frame{.width = 238.0F, .min_width = 174.0F}),
  });
}

[[huxerui::composable]]
View ProjectPane(State<std::size_t> active, State<DockTree> tree, ActivityLog activity) {
  const auto& colors = UseTheme().colors;
  const auto expanded = UseState(true);
  const auto open_file = [active, tree, activity](std::size_t index) {
    if (index == source_folder) {
      return;
    }
    const std::size_t current = active;
    if (current != index) {
      active = index;
      RecordActivity(activity, "Opened " + std::string(project_files[index].path));
    }
    tree = tree->Select("editor");
  };

  return Column {
    Column {
      Text("Harbor", TextRole::Title),
      Text::Format("{} project files", project_files.size()).With(Foreground(colors.on_surface_variant)),
    }.With(Padding(12.0F), Spacing(2.0F)),
    Divider(),
    TreeView<std::size_t>({source_folder, 2, 3}, [colors](std::size_t index) -> View {
      if (index == source_folder) {
        return Text("src").Style({Font::System(13.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}).Key(index);
      }
      const auto& file = project_files[index];
      return Row {
        Text(file.badge).Style({Font::Monospace(10.0F), colors.primary})
            .With(Frame{.width = 28.0F}, Semantics{.hidden = true}),
        Text(file.Name()).With(FontSize(13.0F)),
      }.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center)).Key(index);
    }, [active, expanded](std::size_t index) {
      if (index == source_folder) {
        return TreeItemInfo{.label = "src", .expandable = true, .expanded = expanded};
      }
      const std::size_t selected = active;
      return TreeItemInfo{.label = project_files[index].Name(), .selected = selected == index};
    }).Children([](std::size_t) {
      return std::vector<std::size_t>{0, 1};
    }).OnExpandedChanged([expanded](std::size_t, bool value) {
      expanded = value;
    }).OnSelectionChanged([open_file](std::size_t index, bool selected) {
      if (selected) {
        open_file(index);
      }
    }).OnActivated(open_file)
        .Label("Project files")
        .ItemExtent(32.0F)
        .With(Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container_low));
}

[[huxerui::composable]]
View EditorPane(State<Documents> documents, State<std::size_t> active, ActivityLog activity) {
  const auto& colors = UseTheme().colors;
  const std::size_t index = active;
  const auto& file = project_files[index];
  const auto& document = documents->at(index);
  const TextStyle status_style{Font::System(11.0F), colors.on_surface_variant};
  auto style = UseEnvironment<TextFieldStyle>();
  style.show_label = false;
  style.text_style = {Font::Monospace(13.0F), colors.on_surface};
  style.outlined.border.alpha *= 0.55F;
  style.outlined.corner_radii = {};
  style.padding = EdgeInsets::Symmetric(14.0F, 10.0F);

  return Column {
    Row {
      Text(file.path, TextRole::Label).With(Grow()),
      Text(document.Modified() ? "Modified" : "Saved", TextRole::Label)
          .With(FontSize(11.0F), Foreground(document.Modified() ? colors.primary : colors.on_surface_variant)),
      Button("Revert").OnClick([documents, activity, index] {
        documents.Update([index](Documents& next) {
          auto& current = next[index];
          current.editing = TextEditingValue::FromText(current.saved);
          current.editing.selection = {};
        });
        RecordActivity(activity, "Reverted " + std::string(project_files[index].path));
      }).With(
          Enabled(document.Modified() && !document.editing.composition),
          Background(colors.surface_container_high), Foreground(colors.on_surface),
          Padding(EdgeInsets::Symmetric(10.0F, 4.0F)), Frame{.min_height = 28.0F}
      ),
    }.With(Padding(EdgeInsets::Symmetric(12.0F, 6.0F)), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    Theme(
        ThemeDefinition{}.Set(std::move(style)),
        TextField(document.editing)
            .Label(file.path)
            .Variant(TextFieldVariant::Outlined)
            .InputConfiguration({.multiline = true, .autocorrect = false})
            .OnChanged([documents, index](TextEditingValue next) {
              documents.Update([&](Documents& values) { values[index].editing = std::move(next); });
            })
            .Key(index)
            .With(Grow())
    ),
    Row {
      Text(file.language).Style(status_style),
      Spacer(),
      Text::Format("{} lines  ·  UTF-8", std::ranges::count(document.editing.text, '\n') + 1).Style(status_style),
    }.With(
        Padding(EdgeInsets::Symmetric(12.0F, 6.0F)), Spacing(8.0F),
        Background(colors.surface_container_low), CrossAlign(CrossAxisAlignment::Center)
    ),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface));
}

View Property(std::string_view name, std::string_view value, const ColorScheme& colors) {
  return Row {
    Text(name, TextRole::Label).With(Grow(), Foreground(colors.on_surface_variant)),
    Text(value).With(FontSize(13.0F)),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View InspectorPane(State<Documents> documents, State<std::size_t> active) {
  const auto& colors = UseTheme().colors;
  const auto pinned = UseState(std::optional<std::size_t>{});
  const std::optional<std::size_t>& pinned_file = pinned;
  const std::size_t index = pinned_file ? *pinned_file : static_cast<std::size_t>(active);
  const auto& file = project_files[index];
  const auto& document = documents->at(index);
  return ScrollView(Column {
    Text("FILE PROPERTIES", TextRole::Label).With(FontSize(11.0F), Foreground(colors.on_surface_variant)),
    Text(file.Name(), TextRole::Title),
    Text(file.path).With(FontSize(12.0F), Foreground(colors.on_surface_variant)),
    Divider(),
    Property("Language", file.language, colors),
    Property("Encoding", "UTF-8", colors),
    Property("Size", std::to_string(document.editing.text.size()) + " bytes", colors),
    Property("Draft", document.Modified() ? "Modified" : "Saved", colors),
    Divider(),
    Checkbox("Pin file", pinned_file.has_value()).OnChanged([pinned, index](bool checked) {
      pinned = checked ? std::optional{index} : std::nullopt;
    }),
    Text("Keep this file in the inspector while browsing other files.")
        .With(FontSize(12.0F), Foreground(colors.on_surface_variant)),
  }.With(Padding(12.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Background(colors.surface_container_low));
}

[[huxerui::composable]]
View ActivityPane(ActivityLog activity) {
  const auto& colors = UseTheme().colors;
  const std::vector<std::string>& entries = activity;
  std::vector<View> rows;
  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
    rows.push_back(Text(*entry).Style({Font::Monospace(12.0F), colors.on_surface}));
  }
  if (rows.empty()) {
    rows.push_back(Text("No recent activity").With(FontSize(12.0F), Foreground(colors.on_surface_variant)));
  }
  return Column {
    Row {
      Text("RECENT EVENTS", TextRole::Label).With(Grow(), FontSize(11.0F), Foreground(colors.on_surface_variant)),
      Button("Clear").OnClick([activity] { activity = std::vector<std::string>{}; })
          .With(
              Enabled(!entries.empty()), Background(colors.surface_container_high), Foreground(colors.on_surface),
              Padding(EdgeInsets::Symmetric(10.0F, 4.0F)), Frame{.min_height = 28.0F}
          ),
    }.With(Padding(EdgeInsets::Symmetric(12.0F, 4.0F)), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    ScrollView(Column(std::move(rows)).With(Padding(EdgeInsets::Symmetric(12.0F, 6.0F)), Spacing(5.0F)))
        .With(Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container_low));
}

[[huxerui::composable]]
View Toolbar(State<DockTree> tree, State<Documents> documents, State<std::size_t> active, ActivityLog activity,
             bool compact) {
  const auto& colors = UseTheme().colors;
  const auto panels = UseMenu();
  const auto panels_target = UseViewHandle();
  const std::size_t index = active;
  const auto& document = documents->at(index);
  return Row {
    Column {
      Text("Harbor Studio", TextRole::Title).With(FontSize(compact ? 16.0F : 20.0F)),
      Text("Session workspace", TextRole::Label).With(FontSize(11.0F), Foreground(colors.on_surface_variant)),
    }.With(Grow(), Spacing(2.0F)),
    Button("Panels").With(
        panels_target, Background(colors.surface_container_high), Foreground(colors.on_surface)
    ).OnClick([tree, panels, panels_target, compact] {
      const auto panel_item = [tree, compact](const char* id, const char* label, DockPlacement side) {
        return MenuItem(label, [tree, compact, id, side] {
          tree = tree->Contains(id)
              ? tree->Remove(id)
              : tree->Dock(id, "editor", compact ? DockPlacement::TabAfter : side);
        }).Checked(tree->Contains(id));
      };
      panels.Show(panels_target, {
          panel_item("project", "Explorer", DockPlacement::Left),
          panel_item("inspector", "Inspector", DockPlacement::Right),
          panel_item("activity", "Activity", DockPlacement::Bottom),
          MenuSection{},
          MenuItem("Reset layout", [tree, compact] { tree = InitialLayout(compact); }),
      });
    }),
    Button("Save draft").OnClick([documents, activity, index] {
      documents.Update([index](Documents& next) { next[index].saved = next[index].editing.text; });
      RecordActivity(activity, "Saved " + std::string(project_files[index].path) + " in this session");
    }).With(Enabled(document.Modified() && !document.editing.composition)),
  }.With(
      Padding(EdgeInsets::Symmetric(compact ? 10.0F : 16.0F, 8.0F)), Spacing(8.0F),
      Background(colors.surface), CrossAlign(CrossAxisAlignment::Center)
  );
}

[[huxerui::composable]]
View Workspace() {
  const auto& colors = UseTheme().colors;
  const bool compact = UseViewportClass() != ViewportClass::Expanded;
  const auto desktop_tree = UseState(InitialLayout(false));
  const auto compact_tree = UseState(InitialLayout(true));
  const auto tree = compact ? compact_tree : desktop_tree;
  const auto documents = UseState(InitialDocuments());
  const auto active = UseState<std::size_t>(0);
  const auto activity = UseState(std::vector<std::string>{"Opened src/workspace.cpp", "Harbor workspace ready"});
  auto tabs_style = UseEnvironment<TabsStyle>();
  tabs_style.expand_items = false;
  tabs_style.item_padding = EdgeInsets::Symmetric(compact ? 7.0F : 12.0F, 6.0F);
  tabs_style.minimum_item_width = compact ? 48.0F : 64.0F;

  return Column {
    Toolbar(tree, documents, active, activity, compact),
    Divider(),
    Theme(
        ThemeDefinition{}.Set(std::move(tabs_style)),
        DockLayout(
            {
                DockItem("project", "Explorer", ProjectPane(active, tree, activity)),
                DockItem("editor", "Editor", EditorPane(documents, active, activity)).Closable(false),
                DockItem("inspector", "Inspector", InspectorPane(documents, active)),
                DockItem("activity", "Activity", ActivityPane(activity)),
            },
            tree
        ).OnChanged([tree](DockTree next) { tree = std::move(next); })
            .With(Grow(), Background(colors.surface_container))
    ),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface));
}

} // namespace

View App() {
  const bool dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  return MaterialTheme(dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec(), Workspace());
}

const Application application{
    App,
    {
        .window = {
            .title = "Harbor Studio",
            .initial_size = {1160.0F, 760.0F},
            .minimum_size = Size{360.0F, 480.0F},
        },
        .show_debug_overlay = false,
    }
};
