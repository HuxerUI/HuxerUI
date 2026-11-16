#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace huxerui;

namespace {

enum class StudioGlyph { Harbor, Folder, Code, Document, Settings };

struct ProjectFile {
  std::string_view path;
  std::string_view language;
  StudioGlyph glyph;
  std::string_view source;

  std::string_view Name() const {
    const auto separator = path.rfind('/');
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
  }
};

constexpr std::array project_files{
    ProjectFile{"src/workspace.cpp", "C++", StudioGlyph::Code, R"cpp(#include <huxerui/huxerui.h>
#include "palette.h"

using namespace huxerui;

View Workspace() {
  return Column {
    Text("Harbor Workspace"),
    Text("Create or open a project"),
    Button("New project"),
  }.With(
      Padding(24.0F),
      Spacing(12.0F),
      Background(canvas_color)
  );
}
)cpp"},
    ProjectFile{"src/palette.h", "C++", StudioGlyph::Code, R"cpp(#pragma once

#include <huxerui/color.h>

inline constexpr auto canvas_color =
    huxerui::Color::Rgb(18, 22, 29);
inline constexpr auto accent_color =
    huxerui::Color::Rgb(125, 226, 197);
)cpp"},
    ProjectFile{"harbor.toml", "TOML", StudioGlyph::Settings, R"toml([project]
name = "Harbor"
entry = "src/workspace.cpp"

[window]
width = 960
height = 640
)toml"},
    ProjectFile{"README.md", "Markdown", StudioGlyph::Document, R"md(# Harbor workspace

Move documents across windows.

## Docking

- Drag a tab to reorder it.
- Drop on an edge to split.
- Open a tab in a new window.
- Drag it back into an editor.

Drafts follow their documents.
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

constexpr std::string_view main_layout_id = "main";

struct WorkspaceWindow {
  std::string id;
  DockTree tree;
  WindowHandle handle;
};

struct WorkspaceModel {
  std::vector<WorkspaceWindow> windows;
  std::size_t next_window = 1;
};

struct WorkspaceState {
  State<WorkspaceModel> model;
  State<Documents> documents;
  State<std::size_t> active;
  ActivityLog activity;
};

ThemeSpec StudioTheme() {
  auto theme = FlatDarkThemeSpec();
  auto& colors = theme.colors;
  colors.primary = Color::Rgb(125, 226, 197);
  colors.on_primary = Color::Rgb(12, 35, 29);
  colors.primary_container = Color::Rgb(29, 60, 55);
  colors.on_primary_container = Color::Rgb(164, 240, 218);
  colors.secondary = Color::Rgb(163, 194, 243);
  colors.secondary_container = Color::Rgb(38, 49, 67);
  colors.on_secondary_container = Color::Rgb(209, 225, 249);
  colors.tertiary_container = Color::Rgb(66, 48, 30);
  colors.on_tertiary_container = Color::Rgb(246, 199, 131);
  colors.background = Color::Rgb(13, 16, 21);
  colors.surface = Color::Rgb(18, 22, 29);
  colors.surface_container_low = Color::Rgb(22, 27, 35);
  colors.surface_container = Color::Rgb(25, 31, 40);
  colors.surface_container_high = Color::Rgb(32, 40, 52);
  colors.surface_container_highest = Color::Rgb(40, 50, 64);
  colors.on_surface = Color::Rgb(231, 238, 247);
  colors.on_surface_variant = Color::Rgb(160, 176, 196);
  colors.outline = Color::Rgb(49, 60, 75);
  theme.typography.body_medium = 13.0F;
  theme.typography.label_large = 12.0F;
  theme.shapes.small = 5.0F;
  theme.shapes.medium = 7.0F;
  return theme;
}

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
    return DockTree::Tabs({"src/workspace.cpp", "src/palette.h", "project", "inspector", "activity"});
  }
  return DockTree::Split(Axis::Horizontal, {
      DockTree::Tabs({"project"}).With(Frame{.width = 192.0F, .min_width = 156.0F}),
      DockTree::Split(Axis::Vertical, {
          DockTree::Split(Axis::Horizontal, {
              DockTree::Tabs({"src/workspace.cpp", "src/palette.h"}).With(Grow(1.5F)),
              DockTree::Tabs({"README.md"}).With(Grow()),
          }).With(Grow(), Frame{.min_height = 190.0F}),
          DockTree::Tabs({"activity"}).With(Frame{.height = 136.0F, .min_height = 104.0F}),
      }).With(Grow(), Frame{.min_width = 264.0F}),
      DockTree::Tabs({"inspector"}).With(Frame{.width = 224.0F, .min_width = 174.0F}),
  });
}

std::optional<std::size_t> FileIndex(std::string_view panel) {
  const auto file = std::ranges::find(project_files, panel, &ProjectFile::path);
  if (file == project_files.end()) return std::nullopt;
  return static_cast<std::size_t>(file - project_files.begin());
}

std::string_view DockAnchor(const DockTree& tree, std::size_t active) {
  if (tree.Contains(project_files[active].path)) return project_files[active].path;
  for (const auto& file : project_files) {
    if (tree.Contains(file.path)) return file.path;
  }
  const DockTree* group = &tree;
  while (group->SplitAxis()) group = &group->Children().front();
  return group->Selected();
}

std::optional<std::size_t> SelectedFile(const DockTree& tree) {
  if (const auto file = FileIndex(tree.Selected())) return file;
  for (const auto& child : tree.Children()) {
    if (const auto file = SelectedFile(child)) return file;
  }
  return std::nullopt;
}

const WorkspaceWindow* FindPanelWindow(const WorkspaceModel& model, std::string_view panel) {
  const auto window = std::ranges::find_if(model.windows, [panel](const WorkspaceWindow& entry) {
    return entry.handle.IsOpen() && entry.tree.Contains(panel);
  });
  return window == model.windows.end() ? nullptr : &*window;
}

void RemovePanels(DockTree& tree, const DockTree& moved) {
  for (const auto& panel : moved.Panels()) tree = tree.Remove(panel);
  for (const auto& child : moved.Children()) RemovePanels(tree, child);
}

void PublishLayout(State<WorkspaceModel> state, WorkspaceModel next, std::string_view source_id) {
  const auto source = std::ranges::find(next.windows, source_id, &WorkspaceWindow::id);
  std::optional<WindowHandle> closing;
  if (source != next.windows.end() && source->id != main_layout_id && source->tree.Empty()) {
    closing = source->handle;
  }
  state = std::move(next);
  if (closing) closing->Close();
}

void SetTree(State<WorkspaceModel> state, std::string_view layout_id, DockTree tree) {
  WorkspaceModel next = state;
  const auto window = std::ranges::find(next.windows, layout_id, &WorkspaceWindow::id);
  if (window == next.windows.end() || !window->handle.IsOpen() || window->tree == tree) return;
  window->tree = std::move(tree);
  PublishLayout(state, std::move(next), layout_id);
}

bool ApplyTransfer(WorkspaceModel& model, const DockTransferRequest& request) {
  const auto source = std::ranges::find(model.windows, request.source_layout_id, &WorkspaceWindow::id);
  const auto target = std::ranges::find(model.windows, request.target_layout_id, &WorkspaceWindow::id);
  if (source == model.windows.end() || target == model.windows.end() || source == target ||
      !source->handle.IsOpen() || !target->handle.IsOpen() ||
      !source->tree.Contains(request.panel_id) || target->tree.Contains(request.panel_id)) {
    return false;
  }
  // Resolve the proposed anchor on the current model before changing either tree.
  if (request.target) {
    if (!target->tree.Contains(request.target->panel_id)) return false;
    target->tree = target->tree.Dock(request.panel_id, request.target->panel_id, request.target->placement);
  } else {
    if (!target->tree.Empty()) return false;
    target->tree = DockTree::Tabs({request.panel_id}).With(target->tree.GetFrame(), target->tree.GetGrow());
  }
  source->tree = source->tree.Remove(request.panel_id);
  return true;
}

void MovePanel(WorkspaceState workspace, const DockTransferRequest& request) {
  WorkspaceModel next = workspace.model;
  if (!ApplyTransfer(next, request)) return;
  PublishLayout(workspace.model, std::move(next), request.source_layout_id);
  if (const auto file = FileIndex(request.panel_id)) workspace.active = *file;
  RecordActivity(workspace.activity, "Moved " + request.panel_id + " to " + request.target_layout_id);
}

void MovePanelToMain(WorkspaceState workspace, const std::string& source_id, std::string panel) {
  const auto& windows = workspace.model->windows;
  const auto main = std::ranges::find(windows, main_layout_id, &WorkspaceWindow::id);
  if (main == windows.end() || !main->handle.IsOpen()) return;
  DockTransferRequest request{std::move(panel), source_id, std::string(main_layout_id), {}};
  if (!main->tree.Empty()) {
    request.target = DockTransferRequest::Target{
        std::string(DockAnchor(main->tree, workspace.active)), DockPlacement::TabAfter,
    };
  }
  MovePanel(workspace, request);
}

void RehomePanels(DockTree& target, const DockTree& source) {
  for (const auto& panel : source.Panels()) {
    if (target.Empty()) {
      target = DockTree::Tabs({panel});
    } else if (!target.Contains(panel)) {
      target = target.Dock(panel, DockAnchor(target, 0), DockPlacement::TabAfter);
    }
  }
  for (const auto& child : source.Children()) RehomePanels(target, child);
}

void UseRecoverClosedWindows(State<WorkspaceModel> model) {
  std::vector<std::string> closed_windows;
  for (const auto& window : model->windows) {
    if (!window.handle.IsOpen()) closed_windows.push_back(window.id);
  }
  Lifecycle([model, has_closed_window = !closed_windows.empty()] {
    if (!has_closed_window) return;
    WorkspaceModel next = model;
    const auto receiving = std::ranges::find_if(next.windows, [](const WorkspaceWindow& window) {
      return window.handle.IsOpen();
    });
    if (receiving == next.windows.end()) return;
    bool recovered = false;
    for (const auto& window : next.windows) {
      if (window.handle.IsOpen()) continue;
      RehomePanels(receiving->tree, window.tree);
      recovered = true;
    }
    if (!recovered) return;
    std::erase_if(next.windows, [](const WorkspaceWindow& window) { return !window.handle.IsOpen(); });
    model = std::move(next);
  }, std::move(closed_windows));
}

void TransferToNewWindow(WorkspaceState workspace, std::string source_id, std::string panel, std::string layout_id,
                         WindowHandle opened) {
  // Recheck the source against the current trees after a window request completes.
  WorkspaceModel next = workspace.model;
  next.windows.push_back({layout_id, {}, opened});
  if (!ApplyTransfer(next, DockTransferRequest{panel, source_id, layout_id, {}})) {
    opened.Close();
    return;
  }
  PublishLayout(workspace.model, std::move(next), source_id);
  if (const auto file = FileIndex(panel)) workspace.active = *file;
  RecordActivity(workspace.activity, "Opened " + panel + " in a new window");
}

#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST
Task<void> OpenPanelScene(Task<WindowHandle> opening, WorkspaceState workspace, std::string source_id,
                          std::string panel, std::string layout_id) {
  try {
    auto opened = co_await std::move(opening);
    TransferToNewWindow(workspace, std::move(source_id), std::move(panel), std::move(layout_id), std::move(opened));
  } catch (const std::exception& error) {
    RecordActivity(workspace.activity, "Could not open window: " + std::string(error.what()));
  }
}
#endif

bool CanOpenPanelWindow(const ApplicationHandle& application) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST
  return application.CanOpenWindow();
#else
  return application.SupportsMultipleWindows();
#endif
}

View FloatingWorkspace(WorkspaceState workspace, std::string layout_id);

void OpenPanelWindow(ApplicationHandle application, WorkspaceState workspace, const std::string& source_id,
                     std::string panel) {
  const auto source = FindPanelWindow(workspace.model, panel);
  if (!source || source->id != source_id) return;
  const std::string layout_id = "floating-" + std::to_string(workspace.model->next_window);
  workspace.model.Update([](WorkspaceModel& next) { ++next.next_window; });
  const auto file = FileIndex(panel);
  const std::string title = file ? std::string(project_files[*file].Name()) : "Harbor Panel";
  auto content = [workspace, layout_id] { return FloatingWorkspace(workspace, layout_id); };
  WindowOptions options{
      .title = title + " | Harbor Studio",
      .initial_size = {560.0F, 420.0F},
      .minimum_size = Size{320.0F, 260.0F},
      .chrome_mode = WindowChromeMode::Custom,
      .title_bar_height = 44.0F,
  };

  try {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    auto opening = application.OpenWindowAsync(std::move(content), std::move(options));
    UseApplicationTaskScope().Launch(
        OpenPanelScene(std::move(opening), workspace, source_id, std::move(panel), layout_id));
#else
    auto opened = application.OpenWindow(std::move(content), std::move(options));
    TransferToNewWindow(workspace, source_id, std::move(panel), layout_id, std::move(opened));
#endif
  } catch (const std::exception& error) {
    RecordActivity(workspace.activity, "Could not open window: " + std::string(error.what()));
  }
}

VectorAsset StrokeIcon(Path outline) {
  return VectorAsset::Create({16.0F, 16.0F}, [&](VectorBuilder& builder) {
    builder.StrokePath(std::move(outline), Color::White(),
                       StrokeStyle{.width = 1.5F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
  });
}

const VectorAsset& StudioIcon(StudioGlyph glyph) {
  static const std::array icons{
      StrokeIcon(Path{}.MoveTo({3, 12}).LineTo({8, 3}).LineTo({13, 12}).Close()
                 .MoveTo({2, 14}).LineTo({14, 14}).MoveTo({6, 9}).LineTo({10, 9})),
      StrokeIcon(Path{}.MoveTo({2, 5}).LineTo({2, 3}).LineTo({7, 3}).LineTo({9, 5}).LineTo({14, 5})
                 .LineTo({14, 13}).LineTo({2, 13}).Close().MoveTo({2, 6}).LineTo({14, 6})),
      StrokeIcon(Path{}.MoveTo({5, 5}).LineTo({2, 8}).LineTo({5, 11})
                 .MoveTo({11, 5}).LineTo({14, 8}).LineTo({11, 11}).MoveTo({9, 3}).LineTo({7, 13})),
      StrokeIcon(Path{}.MoveTo({4, 2}).LineTo({9, 2}).LineTo({12, 5}).LineTo({12, 14}).LineTo({4, 14}).Close()
                 .MoveTo({9, 2}).LineTo({9, 5}).LineTo({12, 5}).MoveTo({6, 8}).LineTo({10, 8})
                 .MoveTo({6, 11}).LineTo({9, 11})),
      StrokeIcon(Path{}.MoveTo({2, 5}).LineTo({14, 5}).MoveTo({2, 11}).LineTo({14, 11})
                 .MoveTo({6, 3}).LineTo({6, 7}).MoveTo({10, 9}).LineTo({10, 13})),
  };
  return icons[static_cast<std::size_t>(glyph)];
}

View FileIcon(const ProjectFile& file, const ColorScheme& colors, float size = 15.0F) {
  const Color color = file.glyph == StudioGlyph::Code ? colors.secondary
      : file.glyph == StudioGlyph::Settings ? colors.on_tertiary_container : colors.primary;
  return Image(StudioIcon(file.glyph)).Tint(color).With(Frame{size, size}, Semantics{.hidden = true});
}

View StatusDot(Color color) {
  return Spacer().With(Frame{6.0F, 6.0F}, Grow(0.0F), Background(color), CornerRadius(3.0F), Semantics{.hidden = true});
}

View StudioBrand(const ColorScheme& colors, bool compact) {
  return Row {
    Image(StudioIcon(StudioGlyph::Harbor)).Tint(colors.on_primary_container).With(
        Frame{28.0F, 28.0F}, Padding(5.0F), CornerRadius(7.0F), Background(colors.primary_container),
        Semantics{.hidden = true}
    ),
    compact ? View{} : Text("Harbor Studio")
        .Style({Font::System(14.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
  }.With(Spacing(9.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View ProjectPane(WorkspaceState workspace, std::string layout_id, bool compact) {
  const auto& colors = UseTheme().colors;
  const auto expanded = UseState(true);
  const auto open_file = [workspace, layout_id, compact](std::size_t index) {
    if (index == source_folder) {
      return;
    }
    const std::size_t current = workspace.active;
    const std::string id(project_files[index].path);
    const auto owner = FindPanelWindow(workspace.model, id);
    const bool was_open = owner != nullptr;
    if (owner) {
      const auto window = owner->handle;
      const bool activate = owner->id != layout_id;
      SetTree(workspace.model, owner->id, owner->tree.Select(id));
      if (activate) window.Activate();
    } else {
      const auto& windows = workspace.model->windows;
      const auto target = windows.front().handle.IsOpen() ? windows.begin()
          : std::ranges::find(windows, layout_id, &WorkspaceWindow::id);
      if (target == windows.end()) return;
      const auto& tree = target->tree;
      if (tree.Empty()) {
        SetTree(workspace.model, target->id, DockTree::Tabs({id}));
      } else {
        const auto anchor = DockAnchor(tree, current);
        const auto placement = compact || FileIndex(anchor) ? DockPlacement::TabAfter : DockPlacement::Right;
        SetTree(workspace.model, target->id, tree.Dock(id, anchor, placement));
      }
    }
    workspace.active = index;
    if (!was_open || current != index) RecordActivity(workspace.activity, "Opened " + id);
  };

  return Column {
    Row {
      Image(StudioIcon(StudioGlyph::Folder)).Tint(colors.primary).With(
          Frame{34.0F, 34.0F}, Padding(9.0F), Background(colors.primary_container), CornerRadius(8.0F),
          Semantics{.hidden = true}
      ),
      Column {
        Text("Harbor").Style({Font::System(14.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
        Text::Format("{} files", project_files.size()).With(FontSize(11.0F), Foreground(colors.on_surface_variant)),
      }.With(Spacing(2.0F)),
    }.With(Padding(EdgeInsets::Symmetric(13.0F, 12.0F)), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
    Divider(),
    TreeView<std::size_t>({source_folder, 2, 3}, [documents = workspace.documents, colors](std::size_t index) -> View {
      if (index == source_folder) {
        return Row {
          Image(StudioIcon(StudioGlyph::Folder)).Tint(colors.on_surface_variant)
              .With(Frame{15.0F, 15.0F}, Semantics{.hidden = true}),
          Text("src").Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
        }.With(Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center)).Key(index);
      }
      const auto& file = project_files[index];
      return Row {
        FileIcon(file, colors),
        Text(file.Name()).LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow(), FontSize(12.0F)),
        documents->at(index).Modified() ? StatusDot(colors.on_tertiary_container) : View{},
      }.With(Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center)).Key(index);
    }, [active = workspace.active, expanded](std::size_t index) {
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
        .ItemExtent(30.0F)
        .With(Grow(), Padding(EdgeInsets::Symmetric(0.0F, 6.0F))),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container_low));
}

[[huxerui::composable]]
View EditorPane(State<Documents> documents, State<std::size_t> active, ActivityLog activity, std::size_t index) {
  const auto& colors = UseTheme().colors;
  const auto& file = project_files[index];
  const auto& document = documents->at(index);
  const bool modified = document.Modified();
  const auto name = file.Name();
  const auto directory = file.path.substr(0, file.path.size() - name.size());
  const TextStyle status_style{Font::Monospace(10.5F), colors.on_surface_variant};
  auto style = UseEnvironment<TextFieldStyle>();
  style.show_label = false;
  style.text_style = {Font::Monospace(13.5F), colors.on_surface};
  style.outlined.background = colors.surface;
  style.outlined.border = Color::Transparent();
  style.outlined.hovered_border = Color::Transparent();
  style.outlined.focused_border = colors.outline;
  style.outlined.corner_radii = {};
  style.border_width = 0.0F;
  style.focused_border_width = 1.0F;
  style.caret = colors.primary;
  style.caret_width = 1.0F;
  style.composition = colors.primary;
  style.selection = colors.primary;
  style.selection.alpha = 0.25F;
  style.padding = EdgeInsets::Symmetric(16.0F, 13.0F);
  View revert;
  if (modified) {
    revert = Button("Revert").OnClick([documents, activity, index] {
      documents.Update([index](Documents& next) {
        auto& current = next[index];
        current.editing = TextEditingValue::FromText(current.saved);
        current.editing.selection = {};
      });
      RecordActivity(activity, "Reverted " + std::string(project_files[index].path));
    }).With(
        Enabled(!document.editing.composition), Background(colors.surface_container_high),
        Foreground(colors.on_surface_variant), Padding(EdgeInsets::Symmetric(8.0F, 2.0F)),
        Frame{.min_width = 0.0F, .min_height = 24.0F}
    );
  }

  return Column {
    Row {
      FileIcon(file, colors, 13.0F),
      directory.empty() ? View{} : Text(directory).With(FontSize(11.0F), Foreground(colors.on_surface_variant)),
      Text(name).LineLimit(1).Overflow(TextOverflow::MiddleEllipsis).With(Grow(), FontSize(11.0F)),
      revert,
    }.With(
        Frame{.height = 34.0F}, Padding(EdgeInsets::Symmetric(12.0F, 0.0F)),
        Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)
    ),
    Divider(),
    Theme(
        ThemeDefinition{}.Set(std::move(style)),
        TextField(document.editing)
            .Label(file.path)
            .Variant(TextFieldVariant::Outlined)
            .InputConfiguration({.multiline = true, .autocorrect = false})
            .OnChanged([documents, index](TextEditingValue next) {
              documents.Update([&](Documents& values) { values[index].editing = std::move(next); });
            })
            .On<ViewEvents::FocusChanged>([active, index](bool focused) {
              if (focused) active = index;
            })
            .With(Grow())
    ),
    Divider(),
    Row {
      Text(file.language).Style(status_style),
      Spacer(),
      Text::Format("{} lines", std::ranges::count(document.editing.text, '\n') + 1).Style(status_style),
      Text("UTF-8").Style(status_style),
    }.With(
        Frame{.height = 26.0F}, Padding(EdgeInsets::Symmetric(12.0F, 0.0F)), Spacing(12.0F),
        Background(colors.surface_container_low), CrossAlign(CrossAxisAlignment::Center)
    ),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface));
}

View EditorTabBar(const DockTabBarContext& bar, WorkspaceState workspace, const std::string& layout_id,
                  const ColorScheme& colors, const MenuHandle& menu, const std::function<void(std::string)>& detach) {
  if (std::ranges::none_of(bar.Items(), [](const auto& item) { return FileIndex(item.id).has_value(); })) {
    return bar.DefaultBar();
  }
  const std::size_t active = workspace.active;
  return ScrollView(Row {
    ForEach(bar.Items(), [&](const DockTabBarItem& item) {
      const std::string id(item.id);
      const auto index = FileIndex(id);
      const bool selected = id == bar.Selected();
      const bool modified = index && workspace.documents->at(*index).Modified();
      const Color indicator = index == active ? colors.primary : colors.outline;
      const Color foreground = selected ? colors.on_surface : colors.on_surface_variant;
      View title = Row {
        index ? FileIcon(project_files[*index], colors, 14.0F) : View{},
        Text(item.title).LineLimit(1).Overflow(TextOverflow::MiddleEllipsis)
            .Style({Font::System(12.0F).WithWeight(selected ? FontWeight::Medium : FontWeight::Regular), foreground})
            .With(Frame{.max_width = 118.0F}),
        modified ? StatusDot(colors.on_tertiary_container) : View{},
      }.With(
          Padding(EdgeInsets::Symmetric(9.0F, 6.0F)), Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center),
          Focusable(true), Semantics{.role = SemanticRole::Tab, .label = item.title,
                                   .state_description = modified ? StringVariant("Unsaved changes") : StringVariant{},
                                   .selected = selected}
      ).OnClick([workspace, layout_id, id, index] {
        if (const auto owner = FindPanelWindow(workspace.model, id); owner && owner->id == layout_id) {
          SetTree(workspace.model, layout_id, owner->tree.Select(id));
          if (index) workspace.active = *index;
        }
      });
      if (detach) {
        auto show_menu = [workspace, layout_id, menu, detach, id](Point position) {
          std::vector<MenuEntry> entries{MenuItem("Move to new window", [detach, id] { detach(id); })};
          const auto& windows = workspace.model->windows;
          const auto main = std::ranges::find(windows, main_layout_id, &WorkspaceWindow::id);
          if (layout_id != main_layout_id && main != windows.end() && main->handle.IsOpen()) {
            entries.emplace_back(MenuItem("Move to main window", [workspace, layout_id, id] {
              MovePanelToMain(workspace, layout_id, id);
            }));
          }
          menu.ShowAt(position, std::move(entries));
        };
        title = std::move(title).On<ViewEvents::ContextMenuRequested>(show_menu);
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST
        if (index) {
          title = std::move(title).With(LongPressGesture{}).On<LongPressEvents::Started>(
              [show_menu = std::move(show_menu)](const LongPressEvent& event) { show_menu(event.window_position); }
          );
        }
#endif
      }
      View close;
      if (item.closable) {
        close = Button("×").With(
            Frame{.width = 22.0F, .min_width = 0.0F, .min_height = 25.0F}, Padding(0.0F),
            Background(Color::Transparent()), Foreground(colors.on_surface_variant),
            Semantics{.label = "Close " + std::string(item.title)}
        ).OnClick([workspace, layout_id, id, index] {
          if (const auto owner = FindPanelWindow(workspace.model, id); owner && owner->id == layout_id) {
            auto next = owner->tree.Remove(id);
            if (index == static_cast<std::size_t>(workspace.active)) {
              if (const auto active = SelectedFile(next)) workspace.active = *active;
            }
            SetTree(workspace.model, layout_id, std::move(next));
          }
        });
      }
      return bar.BindTabDrop(id, Column {
        Spacer().With(Frame{.height = 2.0F}, Grow(0.0F), Background(selected ? indicator : Color::Transparent())),
        Row {
          bar.BindTabDrag(id, std::move(title)),
          close,
        }.With(Frame{.height = 34.0F}, CrossAlign(CrossAxisAlignment::Center)),
      }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(selected ? colors.surface : colors.surface_container)))
          .Key(id);
    }),
  }.With(Spacing(1.0F), Semantics{.role = SemanticRole::TabList}))
      .ScrollAxis(Axis::Horizontal)
      .With(ScrollBar(), Background(colors.background));
}

View Property(std::string_view name, std::string_view value, const ColorScheme& colors) {
  return Row {
    Text(name).With(Grow(), FontSize(12.0F), Foreground(colors.on_surface_variant)),
    Text(value).Style({Font::Monospace(11.0F), colors.on_surface}),
  }.With(Frame{.min_height = 26.0F}, Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View InspectorPane(State<Documents> documents, State<std::size_t> active) {
  const auto& colors = UseTheme().colors;
  const auto pinned = UseState(std::optional<std::size_t>{});
  const std::optional<std::size_t>& pinned_file = pinned;
  const std::size_t index = pinned_file ? *pinned_file : static_cast<std::size_t>(active);
  const auto& file = project_files[index];
  const auto& document = documents->at(index);
  const Color draft_color = document.Modified() ? colors.on_tertiary_container : colors.on_primary_container;
  return ScrollView(Column {
    Column {
      Row {
        Row {FileIcon(file, colors, 18.0F)}.With(Padding(8.0F), Background(colors.surface_container_highest),
                                               CornerRadius(7.0F)),
        Column {
          Text(file.Name()).LineLimit(1).Overflow(TextOverflow::MiddleEllipsis)
              .Style({Font::System(14.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
          Text(file.path).LineLimit(1).Overflow(TextOverflow::MiddleEllipsis)
              .Style({Font::Monospace(10.0F), colors.on_surface_variant}),
        }.With(Grow(), Spacing(4.0F)),
      }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
      Row {
        StatusDot(draft_color),
        Text(document.Modified() ? "Unsaved draft" : "Saved").With(FontSize(11.0F), Foreground(draft_color)),
      }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
    }.With(
        Padding(11.0F), Spacing(10.0F), Background(colors.surface_container_high),
        Border{colors.outline}, CornerRadius(8.0F)
    ),
    Column {
      Text("Document").Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
      Divider(),
      Property("Language", file.language, colors),
      Property("Encoding", "UTF-8", colors),
      Property("Lines", std::to_string(std::ranges::count(document.editing.text, '\n') + 1), colors),
      Property("Size", std::to_string(document.editing.text.size()) + " bytes", colors),
    }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    Checkbox("Pin file", pinned_file.has_value()).OnChanged([pinned, index](bool checked) {
      pinned = checked ? std::optional{index} : std::nullopt;
    }),
  }.With(Padding(13.0F), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Background(colors.surface_container_low));
}

[[huxerui::composable]]
View ActivityPane(ActivityLog activity) {
  const auto& colors = UseTheme().colors;
  const std::vector<std::string>& entries = activity;
  std::vector<View> rows;
  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
    rows.push_back(Row {
      StatusDot(entry == entries.rbegin() ? colors.primary : colors.outline),
      Text(*entry).LineLimit(1).Overflow(TextOverflow::Ellipsis)
          .Style({Font::Monospace(11.0F), colors.on_surface}).With(Grow()),
    }.With(Spacing(9.0F), CrossAlign(CrossAxisAlignment::Center)));
  }
  if (rows.empty()) {
    rows.push_back(Text("No recent activity").With(FontSize(12.0F), Foreground(colors.on_surface_variant)));
  }
  return Column {
    Row {
      Text::Format(entries.size() == 1 ? "{} event" : "{} events", entries.size())
          .With(Grow(), FontSize(11.0F), Foreground(colors.on_surface_variant)),
      Button("Clear").OnClick([activity] { activity = std::vector<std::string>{}; })
          .With(
              Enabled(!entries.empty()), Background(colors.surface_container_high),
              Foreground(colors.on_surface_variant),
              Padding(EdgeInsets::Symmetric(8.0F, 2.0F)), Frame{.min_width = 0.0F, .min_height = 24.0F}
          ),
    }.With(Padding(EdgeInsets::Symmetric(12.0F, 4.0F)), CrossAlign(CrossAxisAlignment::Center)),
    Divider(),
    ScrollView(Column(std::move(rows)).With(
        Padding(EdgeInsets::Symmetric(12.0F, 8.0F)), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)
    )).With(Grow(), Background(colors.surface)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container_low));
}

[[huxerui::composable]]
View Toolbar(WorkspaceState workspace, bool compact) {
  const auto& colors = UseTheme().colors;
  const auto panels = UseMenu();
  const auto panels_target = UseViewHandle();
  const std::size_t index = workspace.active;
  const auto& document = workspace.documents->at(index);
  auto save_style = UseEnvironment<ButtonStyle>();
  save_style.background = colors.primary;
  save_style.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_primary};
  save_style.disabled_background = colors.surface_container_high;
  save_style.disabled_label = colors.on_surface_variant;
  save_style.disabled_label.alpha = 0.66F;
  return WindowTitleBar {
    StudioBrand(colors, compact),
    Spacer().With(Grow()),
    Button("Panels").With(
        panels_target, Frame{.height = 28.0F}, Padding(EdgeInsets::Symmetric(10.0F, 0.0F)),
        Background(colors.surface_container_high), Foreground(colors.on_surface), Border{colors.outline}
    ).OnClick([workspace, panels, panels_target, compact] {
      const auto panel_item = [workspace, compact](const char* id, const char* label, DockPlacement side) {
        return MenuItem(label, [workspace, compact, id, side] {
          const auto owner = FindPanelWindow(workspace.model, id);
          const auto& tree = workspace.model->windows.front().tree;
          if (owner && owner->id != main_layout_id) {
            owner->handle.Activate();
          } else if (owner) {
            SetTree(workspace.model, main_layout_id, tree.Remove(id));
          } else if (tree.Empty()) {
            SetTree(workspace.model, main_layout_id, DockTree::Tabs({id}));
          } else {
            const auto placement = compact ? DockPlacement::TabAfter : side;
            SetTree(workspace.model, main_layout_id, tree.Dock(id, DockAnchor(tree, workspace.active), placement));
          }
        }).Checked(FindPanelWindow(workspace.model, id) != nullptr);
      };
      panels.Show(panels_target, {
          panel_item("project", "Explorer", DockPlacement::Left),
          panel_item("inspector", "Inspector", DockPlacement::Right),
          panel_item("activity", "Activity", DockPlacement::Bottom),
          MenuSection{},
          MenuItem("Reset layout", [workspace, compact] {
            auto tree = InitialLayout(compact);
            const auto& windows = workspace.model->windows;
            for (std::size_t index = 1; index < windows.size(); ++index) {
              RemovePanels(tree, windows[index].tree);
            }
            SetTree(workspace.model, main_layout_id, std::move(tree));
          }),
      });
    }),
    Theme(
        ThemeDefinition{}.Set(std::move(save_style)),
        Button("Save draft").OnClick([workspace, index] {
          workspace.documents.Update([index](Documents& next) { next[index].saved = next[index].editing.text; });
          RecordActivity(workspace.activity, "Saved " + std::string(project_files[index].path) + " in this session");
        }).With(
            Enabled(FindPanelWindow(workspace.model, project_files[index].path) &&
                    document.Modified() && !document.editing.composition),
            Frame{.height = 28.0F}, Padding(EdgeInsets::Symmetric(10.0F, 0.0F))
        )
    ),
  }.With(
      Frame{.min_height = 46.0F}, Padding(EdgeInsets::Symmetric(11.0F, 4.0F)), Spacing(7.0F),
      Background(colors.surface_container_low)
  );
}

View StudioStatus(const WorkspaceState& workspace, const ColorScheme& colors) {
  const Documents& documents = workspace.documents;
  const auto drafts = std::ranges::count_if(documents, &Document::Modified);
  const auto windows = std::ranges::count_if(workspace.model->windows, [](const WorkspaceWindow& window) {
    return window.handle.IsOpen();
  });
  const Color draft_color = drafts == 0 ? colors.primary : colors.on_tertiary_container;
  const auto draft_status = drafts == 0 ? "All changes saved"
      : std::to_string(drafts) + (drafts == 1 ? " unsaved draft" : " unsaved drafts");
  return Row {
    StatusDot(draft_color),
    Text(draft_status).With(FontSize(11.0F), Foreground(colors.on_surface_variant)),
    Spacer(),
    windows > 1 ? Text::Format("{} windows", windows).With(FontSize(11.0F), Foreground(colors.on_surface_variant))
                : View{},
  }.With(
      Frame{.height = 26.0F}, Padding(EdgeInsets::Symmetric(12.0F, 0.0F)), Spacing(7.0F),
      Background(colors.surface_container_low), CrossAlign(CrossAxisAlignment::Center)
  );
}

[[huxerui::composable]]
View WorkspaceDock(WorkspaceState workspace, std::string layout_id, bool compact) {
  const auto& colors = UseTheme().colors;
  const auto application = UseApplication();
  const auto menu = UseMenu();
  const auto& windows = workspace.model->windows;
  const auto window = std::ranges::find(windows, layout_id, &WorkspaceWindow::id);
  if (window == windows.end()) return {};

  const bool main = layout_id == main_layout_id;
  const auto owns = [&](std::string_view panel) {
    const auto owner = FindPanelWindow(workspace.model, panel);
    return owner ? owner == &*window : main;
  };
  std::vector<DockItem> items;
  if (owns("project")) items.emplace_back("project", "Explorer", ProjectPane(workspace, layout_id, compact));
  if (owns("inspector")) {
    items.emplace_back("inspector", "Inspector", InspectorPane(workspace.documents, workspace.active));
  }
  if (owns("activity")) items.emplace_back("activity", "Activity", ActivityPane(workspace.activity));
  for (std::size_t index = 0; index < project_files.size(); ++index) {
    const auto& file = project_files[index];
    if (owns(file.path)) {
      items.emplace_back(std::string(file.path), file.Name(),
                         EditorPane(workspace.documents, workspace.active, workspace.activity, index));
    }
  }

  std::function<void(std::string)> detach;
  if (CanOpenPanelWindow(application)) {
    detach = [application, workspace, layout_id](std::string panel) {
      OpenPanelWindow(application, workspace, layout_id, std::move(panel));
    };
  }
  auto layout = DockLayout(std::move(items), window->tree)
      .Workspace("harbor", layout_id)
      .TabBar([workspace, layout_id, colors, menu, detach](const DockTabBarContext& bar) {
        return EditorTabBar(bar, workspace, layout_id, colors, menu, detach);
      })
      .OnChanged([state = workspace.model, layout_id](DockTree next) { SetTree(state, layout_id, std::move(next)); })
      .On<DockEvents::TransferRequested>([workspace](const DockTransferRequest& request) {
        MovePanel(workspace, request);
      });
  if (detach) layout = std::move(layout).On<DockEvents::DetachRequested>(std::move(detach));

  auto tabs_style = UseEnvironment<TabsStyle>();
  tabs_style.background = colors.surface_container;
  tabs_style.label_style = {Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_surface_variant};
  tabs_style.selected_label = colors.on_surface;
  tabs_style.indicator = colors.primary;
  tabs_style.indicator_height = 1.0F;
  tabs_style.divider_color = colors.outline;
  tabs_style.divider_height = 1.0F;
  tabs_style.expand_items = false;
  tabs_style.item_padding = EdgeInsets::Symmetric(compact ? 7.0F : 11.0F, 5.0F);
  tabs_style.minimum_item_width = compact ? 48.0F : 64.0F;
  tabs_style.minimum_height = 36.0F;
  auto tree_style = UseEnvironment<TreeViewStyle>();
  tree_style.indentation = 16.0F;
  tree_style.item_padding = 10.0F;
  tree_style.foreground = colors.on_surface_variant;
  tree_style.focus_indicator = colors.primary;
  tree_style.selected_background = colors.primary_container;
  tree_style.active_background = colors.surface_container_high;
  auto checkbox_style = UseEnvironment<CheckboxStyle>();
  checkbox_style.size = 16.0F;
  checkbox_style.checked_background = colors.primary;
  checkbox_style.checkmark = colors.on_primary;
  checkbox_style.unchecked_border = Border{colors.on_surface_variant, 1.0F};
  return Theme(
      ThemeDefinition{}.Set(std::move(tabs_style)).Set(std::move(tree_style))
          .Set(std::move(checkbox_style)).Set(DividerStyle{colors.outline, 1.0F}),
      std::move(layout).With(Background(colors.background), CornerRadius(8.0F), ClipChildren{})
  );
}

[[huxerui::composable]]
View Workspace() {
  const auto& colors = UseTheme().colors;
  const bool compact = UseViewportClass() != ViewportClass::Expanded;
  const WorkspaceState workspace = *UseService<WorkspaceState>();
  const auto window = UseWindow();
  Lifecycle([model = workspace.model, window, compact] {
    const auto& windows = model->windows;
    if (std::ranges::find(windows, window, &WorkspaceWindow::handle) != windows.end()) return;

    WorkspaceModel next = model;
    const auto main = std::ranges::find(next.windows, main_layout_id, &WorkspaceWindow::id);
    if (main == next.windows.end()) {
      auto tree = InitialLayout(compact);
      for (const auto& other : next.windows) {
        if (other.handle.IsOpen()) RemovePanels(tree, other.tree);
      }
      next.windows.insert(next.windows.begin(), {std::string(main_layout_id), std::move(tree), window});
    } else if (!main->handle.IsOpen()) {
      main->handle = window;
    } else {
      const auto file = std::ranges::find_if(project_files, [&next](const ProjectFile& candidate) {
        return !FindPanelWindow(next, candidate.path);
      });
      auto tree = file == project_files.end() ? DockTree{} : DockTree::Tabs({std::string(file->path)});
      auto layout_id = "scene-" + std::to_string(next.next_window++);
      next.windows.push_back({std::move(layout_id), std::move(tree), window});
    }
    model = std::move(next);
  }, window, compact);

  const auto& windows = workspace.model->windows;
  const auto current = std::ranges::find(windows, window, &WorkspaceWindow::handle);
  if (current == windows.end()) return {};
  if (current->id != main_layout_id) return FloatingWorkspace(workspace, current->id);

  UseRecoverClosedWindows(workspace.model);
  return Column {
    Toolbar(workspace, compact),
    Divider(),
    WorkspaceDock(workspace, std::string(main_layout_id), compact).With(Padding(8.0F), Grow()),
    Divider(),
    StudioStatus(workspace, colors),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.background));
}

[[huxerui::composable]]
View FloatingWorkspace(WorkspaceState workspace, std::string layout_id) {
  UseRecoverClosedWindows(workspace.model);
  const auto theme = StudioTheme();
  const auto& colors = theme.colors;
  return FlatTheme(theme, Column {
    WindowTitleBar {
      StudioBrand(colors, false),
      Spacer().With(Grow()),
    }.With(
        Frame{.min_height = 44.0F}, Padding(EdgeInsets::Symmetric(11.0F, 4.0F)),
        Background(colors.surface_container_low)
    ),
    Divider(),
    WorkspaceDock(workspace, layout_id, true).With(Padding(7.0F), Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.background)));
}

} // namespace

View App() {
  return FlatTheme(StudioTheme(), Workspace());
}

const Application application{
    App,
    {
        .window = {
            .title = "Harbor Studio",
            .initial_size = {1280.0F, 800.0F},
            .minimum_size = Size{1000.0F, 480.0F},
            .chrome_mode = WindowChromeMode::Custom,
            .title_bar_height = 46.0F,
        },
        .show_debug_overlay = false,
        .application_hooks = {
            [](ApplicationContext& context) {
              context.Provide(std::make_shared<WorkspaceState>(WorkspaceState{
                  State{WorkspaceModel{}},
                  State{InitialDocuments()},
                  State<std::size_t>{0},
                  ActivityLog{std::vector<std::string>{"Opened src/workspace.cpp", "Harbor workspace ready"}},
              }));
            },
        },
    }
};
