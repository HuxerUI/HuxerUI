#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>
#include <thread>

#include "testing_internal.h"
#include "internal_access.h"
#include "runtime/mounted_node_internal.h"

namespace huxerui::detail {
namespace {

constexpr std::size_t diagnostic_node_limit = 12;
constexpr std::size_t diagnostic_text_limit = 160;

std::string Quote(std::string_view value) {
  std::ostringstream out;
  out << '"';
  std::size_t length = std::min(value.size(), diagnostic_text_limit);
  while (length < value.size() && length && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80) --length;
  for (unsigned char c : value.substr(0, length)) {
    if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
    else if (c < 32 || c == 127) out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << int(c) << std::dec;
    else out << static_cast<char>(c);
  }
  if (length < value.size()) out << "...";
  out << '"';
  return out.str();
}

std::string NodeType(NodeKind kind) {
  switch (kind) {
    case NodeKind::Text: return "Text";
    case NodeKind::Button: return "Button";
    case NodeKind::IconButton: return "IconButton";
    case NodeKind::Chip: return "Chip";
    case NodeKind::Divider: return "Divider";
    case NodeKind::TextField: return "TextField";
    case NodeKind::Checkbox: return "Checkbox";
    case NodeKind::RadioButton: return "RadioButton";
    case NodeKind::Switch: return "Switch";
    case NodeKind::ProgressCircle: return "ProgressCircle";
    case NodeKind::ProgressBar: return "ProgressBar";
    case NodeKind::Slider: return "Slider";
    case NodeKind::Image: return "Image";
    case NodeKind::PlatformView: return "PlatformView";
    case NodeKind::Canvas: return "Canvas";
    case NodeKind::Spacer: return "Spacer";
    case NodeKind::SelectionArea: return "SelectionArea";
    case NodeKind::Layout: return "Layout";
    case NodeKind::ScrollView: return "ScrollView";
    case NodeKind::VirtualLayout: return "VirtualLayout";
    case NodeKind::Scope: return "Scope";
    case NodeKind::Environment: return "Environment";
  }
  throw std::logic_error("HuxerUI testing encountered an unknown node kind");
}

Rect Intersection(Rect a, Rect b) {
  const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
  return {x, y, std::max(0.0F, std::min(a.x + a.width, b.x + b.width) - x),
          std::max(0.0F, std::min(a.y + a.height, b.y + b.height) - y)};
}

void ValidateDuration(double duration) {
  if (!std::isfinite(duration) || duration < 0) throw std::invalid_argument("HuxerUI testing duration is invalid");
}

void ValidatePoint(Point point) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y))
    throw std::invalid_argument("HuxerUI testing input point must be finite");
}

void ValidatePumpOptions(const testing::UiPumpOptions& options, double now) {
  ValidateDuration(options.timeout.count());
  ValidateDuration(options.step.count());
  ValidateDuration(now + options.timeout.count());
  if (options.step.count() <= 0 || !options.maximum_frames)
    throw std::invalid_argument("HuxerUI testing requires a positive virtual step and frame budget");
}

} // namespace

class UiTestSession {
public:
  explicit UiTestSession(const Application& application, const testing::UiTestOptions& options)
      : queue(std::make_shared<UiTestQueue>()), adapter(queue, options), owner(std::this_thread::get_id()),
        maximum_callbacks(options.maximum_callbacks_per_frame) {
    try {
      if (!maximum_callbacks) throw std::invalid_argument("HuxerUI testing callback budget must be positive");
      runtime = std::make_unique<Runtime>(application, adapter, options.activation);
      metrics.viewport = options.viewport;
      metrics.safe_area = options.safe_area;
      runtime->SetWindowMetrics(metrics);
      runtime->UpdateResourceConfiguration(options.resources);
      Pump(0);
    } catch (...) {
      Shutdown();
      throw;
    }
  }

  ~UiTestSession() { Shutdown(); }

  void Shutdown() noexcept {
    busy = true;
    runtime.reset();
    // Runtime teardown can enqueue transport cancellation and disposal while the adapter is still alive.
    for (std::size_t processed = 0; processed < maximum_callbacks; ++processed) {
      std::function<void()> callback;
      {
        std::scoped_lock lock(queue->mutex);
        if (queue->callbacks.empty()) break;
        callback = std::move(queue->callbacks.front());
        queue->callbacks.pop_front();
      }
      try {
        callback();
      } catch (...) {
        // Destruction cannot throw, and initialization cleanup must preserve the original exception.
      }
    }
    std::deque<std::function<void()>> abandoned;
    {
      std::scoped_lock lock(queue->mutex);
      queue->closed = true;
      abandoned.swap(queue->callbacks);
    }
  }

  void Check(bool mutation = false) const {
    if (owner != std::this_thread::get_id()) throw std::logic_error("HuxerUI testing requires its creating thread");
    if (busy || (mutation && evaluating)) throw std::logic_error("HuxerUI testing operations must not reenter");
    if (failed) throw testing::UiTestFailure("HuxerUI testing session failed during frame/input processing");
  }

  template <class Function> auto Mutate(Function&& function) {
    Check(true);
    busy = true;
    try {
      if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
        function();
        busy = false;
      } else {
        auto result = function();
        busy = false;
        return result;
      }
    } catch (...) {
      busy = false;
      failed = true;
      throw;
    }
  }

  void Pump(double duration) {
    Check(true);
    ValidateDuration(duration);
    ValidateDuration(adapter.time + duration);
    Mutate([&] {
      std::deque<std::function<void()>> ready;
      {
        std::scoped_lock lock(queue->mutex);
        if (queue->callbacks.size() > maximum_callbacks)
          throw testing::UiTestFailure("HuxerUI testing frame callback budget exceeded");
        ready.swap(queue->callbacks);
      }
      adapter.time += duration;
      // A frame consumes the previous wakeup. Tasks still pending reassert their deadline during BuildFrame.
      adapter.frame_deadline.reset();
      for (auto& callback : ready) callback();
      const auto& commit = runtime->BuildFrame();
      if (commit.next_frame_deadline) adapter.RequestFrameAt(*commit.next_frame_deadline);
      nodes.clear();
      if (const auto* root = InternalAccess::MountedRoot(*runtime)) {
        Collect(*root, {}, {0, 0, metrics.viewport.width, metrics.viewport.height}, true);
      }
      semantics = commit.semantic_frame;
      semantic_index.clear();
      if (semantics) {
        semantic_index.reserve(semantics->nodes.size());
        for (const auto& node : semantics->nodes) semantic_index.emplace(node.id, &node);
      }
      structure = CaptureUiStructure(commit, semantic_index);
    });
  }

  struct ObservedNode {
    testing::UiNodeInfo info;
    std::uint64_t identity;
    std::optional<std::size_t> parent;
    Point center;
  };

  void Collect(const MountedNode& node, std::optional<std::size_t> parent, Rect clip, bool participates) {
    const auto index = nodes.size();
    testing::UiNodeInfo info;
    info.type = NodeType(node.kind);
    info.key = node.key;
    info.enabled = node.interaction.enabled;
    info.focused = node.interaction.focused;
    info.size = {node.bounds.width, node.bounds.height};
    info.bounds = TransformBounds(node.presentation.resolved_transform, node.bounds);
    info.participates_in_layout = participates && node.participates_in_layout;
    info.visible_bounds = info.participates_in_layout ? Intersection(clip, info.bounds) : Rect{};
    info.in_viewport = !info.visible_bounds.IsEmpty();
    switch (node.kind) {
      case NodeKind::Text: case NodeKind::Button: case NodeKind::Chip:
      case NodeKind::Checkbox: case NodeKind::RadioButton: case NodeKind::Switch:
        info.text = node.text.PlainText(); break;
      default: break;
    }
    if (node.kind == NodeKind::TextField) {
      for (const auto& entry : node.extensions) {
        const auto client = entry.extension->GetTextInputClient();
        if (!client || client->Configuration().secure) continue;
        // Built-in TextField supports its own current session (including inactive zero) without starting input.
        const auto context = client->QueryTextInputContext(client->State().session_id, 0, 0);
        if (context.result_code == TextInputResultCode::Ok && context.slice_start == 0) info.value = context.text;
      }
    }
    const Point center = node.presentation.resolved_transform.Apply(
        {node.bounds.x + node.bounds.width * 0.5F, node.bounds.y + node.bounds.height * 0.5F});
    nodes.push_back({std::move(info), node.identity, parent, center});
    for (const auto& child_clip : node.render_node.child_clips) {
      if (const auto* rect = std::get_if<PushClipCommand>(&child_clip))
        clip = Intersection(clip, TransformBounds(node.presentation.resolved_transform, rect->rect));
    }
    for (const auto& child : node.children) Collect(*child, index, clip, nodes[index].info.participates_in_layout);
  }

  std::vector<std::size_t> Resolve(const std::vector<testing::UiSelector>& selectors,
                                 std::string_view operation = "Query", bool require_one = false) const {
    Check();
    std::optional<std::size_t> scope;
    std::vector<std::size_t> matches;
    for (std::size_t depth = 0; depth < selectors.size(); ++depth) {
      matches.clear();
      std::vector<std::size_t> candidates;
      for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (scope) {
          auto parent = nodes[i].parent;
          while (parent && parent != scope) parent = nodes[*parent].parent;
          if (!parent) continue;
        }
        if (candidates.size() < diagnostic_node_limit) candidates.push_back(i);
        if (selectors[depth].matches_(nodes[i].info)) matches.push_back(i);
      }
      if ((require_one || depth + 1 < selectors.size()) && matches.size() != 1) {
        std::vector<std::string> descriptions;
        const auto& shown = matches.empty() ? candidates : matches;
        for (auto index : shown) {
          if (descriptions.size() == diagnostic_node_limit) break;
          descriptions.push_back(Describe(nodes[index].info));
        }
        QueryFailure(operation, Chain(selectors, selectors.size() - 1) +
            "\nFailed selector: " + selectors[depth].description_, matches.size(), descriptions);
      }
      if (depth + 1 < selectors.size()) {
        scope = matches.front();
      }
    }
    return matches;
  }

  ObservedNode One(const std::vector<testing::UiSelector>& selectors, std::string_view operation = "One") const {
    const auto result = Resolve(selectors, operation, true);
    return nodes[result.front()];
  }

  std::vector<const SemanticNode*> Resolve(const std::vector<testing::UiSemanticSelector>& selectors,
                                         std::string_view operation = "Query", bool require_one = false) const {
    Check();
    std::vector<const SemanticNode*> matches;
    std::optional<SemanticNodeId> scope;
    for (std::size_t depth = 0; depth < selectors.size(); ++depth) {
      matches.clear();
      std::vector<const SemanticNode*> candidates;
      if (semantics) {
        std::function<void(SemanticNodeId, bool)> visit = [&](SemanticNodeId id, bool descendant) {
          const auto& node = SemanticNodeAt(semantic_index, id);
          if (descendant && candidates.size() < diagnostic_node_limit) candidates.push_back(&node);
          if (descendant && selectors[depth].matches_(node)) matches.push_back(&node);
          for (auto child : node.children) visit(child, true);
        };
        visit(scope.value_or(semantics->root), !scope.has_value());
      }
      if ((require_one || depth + 1 < selectors.size()) && matches.size() != 1) {
        std::vector<std::string> descriptions;
        for (const auto* node : matches.empty() ? candidates : matches) {
          if (descriptions.size() == diagnostic_node_limit) break;
          std::ostringstream out;
          out << "Semantic role=" << static_cast<int>(node->role) << " identifier=" << Quote(node->identifier)
              << " label=" << (node->secure ? "<redacted>" : Quote(node->label))
              << " bounds=(" << node->bounds.x << ',' << node->bounds.y << ',' << node->bounds.width << ','
              << node->bounds.height << ") enabled=" << node->enabled << " offscreen=" << node->offscreen;
          descriptions.push_back(out.str());
        }
        QueryFailure(operation, Chain(selectors, selectors.size() - 1) +
            "\nFailed selector: " + selectors[depth].description_, matches.size(), descriptions);
      }
      if (depth + 1 < selectors.size()) {
        scope = matches.front()->id;
      }
    }
    return matches;
  }

  const SemanticNode& One(const std::vector<testing::UiSemanticSelector>& selectors,
                          std::string_view operation = "One") const {
    auto matches = Resolve(selectors, operation, true);
    return *matches.front();
  }

  template <class Selector> static std::string Chain(const std::vector<Selector>& selectors, std::size_t depth) {
    std::string result;
    for (std::size_t i = 0; i <= depth; ++i) {
      if (i) result += " -> ";
      result += selectors[i].description_;
    }
    return result;
  }

  static std::string Describe(const testing::UiNodeInfo& info) {
    std::ostringstream out;
    out << info.type;
    if (info.key) std::visit([&](const auto& key) {
      out << " key=";
      if constexpr (std::same_as<std::decay_t<decltype(key)>, std::string>) out << Quote(key);
      else out << key;
    }, *info.key);
    if (info.text) out << " text=" << Quote(*info.text);
    out << " bounds=(" << info.bounds.x << ',' << info.bounds.y << ',' << info.bounds.width << ','
        << info.bounds.height << ") enabled=" << info.enabled << " participating=" << info.participates_in_layout;
    return out.str();
  }

  [[noreturn]] void QueryFailure(std::string_view operation, const std::string& selector, std::size_t count,
                                const std::vector<std::string>& candidates) const {
    std::ostringstream message;
    message.imbue(std::locale::classic());
    message << "HuxerUI testing " << operation << " expected one node, found " << count
            << "\nQuery: " << selector << "\nViewport: " << metrics.viewport.width << 'x' << metrics.viewport.height
            << "; time=" << adapter.time << "; semantic_revision=" << (semantics ? semantics->revision : 0)
            << (count ? "\nCandidates:" : "\nScope excerpt:");
    for (const auto& candidate : candidates) message << "\n  " << candidate;
    if (count > candidates.size()) message << "\n  ... " << count - candidates.size() << " more matches";
    throw testing::UiTestFailure(message.str());
  }

  Point TargetPoint(const std::vector<testing::UiSelector>& selectors, std::string_view operation) const {
    const auto node = One(selectors, operation);
    if (!node.info.enabled || !node.info.in_viewport)
      throw testing::UiTestFailure("HuxerUI testing " + std::string(operation) +
          " requires enabled viewport geometry\nQuery: " + Chain(selectors, selectors.size() - 1) +
          "\n  " + Describe(node.info));
    const auto& visible = node.info.visible_bounds;
    return visible.Contains(node.center) ? node.center
        : Point{visible.x + visible.width * 0.5F, visible.y + visible.height * 0.5F};
  }

  void Pointer(const PointerEvent& event) {
    ValidatePoint(event.position);
    Mutate([&] { runtime->HandlePointerEvent(event); });
    if (event.type == PointerEventType::Down) active_pointers.insert(event.pointer_id);
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel)
      active_pointers.erase(event.pointer_id);
  }

  template <class Function> void Gesture(PointerEvent& event, Function&& function) {
    Check(true);
    if (active_pointers.contains(event.pointer_id))
      throw testing::UiTestFailure("HuxerUI testing gesture pointer is already active");
    try {
      function();
    } catch (...) {
      // Preserve the original failure even when cancellation itself throws in application code.
      event.type = PointerEventType::Cancel;
      event.pressed_buttons = PointerButton::None;
      try { runtime->HandlePointerEvent(event); } catch (...) {}
      active_pointers.erase(event.pointer_id);
      throw;
    }
  }

  void Tap(Point point, testing::UiPointerOptions options) {
    Check(true);
    ValidatePoint(point);
    if (!Rect{0, 0, metrics.viewport.width, metrics.viewport.height}.Contains(point))
      throw testing::UiTestFailure("HuxerUI testing tap point is outside the viewport");
    PointerEvent event{.type = PointerEventType::Down, .pointer_id = options.pointer_id, .position = point,
                       .device_kind = options.device_kind, .changed_button = PointerButton::Primary,
                       .pressed_buttons = PointerButton::Primary};
    Gesture(event, [&] {
      Pointer(event);
      Pump(0);
      event.type = PointerEventType::Up;
      event.pressed_buttons = PointerButton::None;
      Pointer(event);
      Pump(0);
    });
  }

  TextInputSessionId RequireEditor(const std::vector<testing::UiSelector>& selectors) const {
    Check(true);
    const auto node = One(selectors, "Edit");
    if (!adapter.input_state || InternalAccess::FocusedNodeIdentity(*runtime) != node.identity)
      throw testing::UiTestFailure("HuxerUI testing editor must own the active text-input session");
    return adapter.input_state->session_id;
  }

  void Edit(const std::vector<testing::UiSelector>& selectors, TextInputCommand command, bool replace = false) {
    const auto session = RequireEditor(selectors);
    std::vector<TextInputCommand> commands;
    if (replace) {
      const auto context = runtime->QueryTextInputContext(session, 0, 0);
      if (context.result_code != TextInputResultCode::Ok)
        throw testing::UiTestFailure("HuxerUI testing editor cannot report its text extent");
      commands.push_back({
        .kind = TextInputCommandKind::FinishComposition, .target = {}, .selection_after = {}, .text = {},
      });
      command.target = TextRange{0, context.total_length};
    }
    commands.push_back(std::move(command));
    const auto result = Mutate([&] { return runtime->HandleTextInputCommands({session, std::move(commands)}); });
    if (result.result_code != TextInputResultCode::Ok)
      throw testing::UiTestFailure("HuxerUI testing text-input command was rejected");
    Pump(0);
  }

  std::shared_ptr<UiTestQueue> queue;
  TestingPlatformAdapter adapter;
  std::unique_ptr<Runtime> runtime;
  WindowMetrics metrics;
  std::thread::id owner;
  std::size_t maximum_callbacks;
  bool busy = false;
  bool evaluating = false;
  bool failed = false;
  std::vector<ObservedNode> nodes;
  std::set<std::int64_t> active_pointers;
  std::shared_ptr<const SemanticFrame> semantics;
  UiSemanticIndex semantic_index;
  std::shared_ptr<const UiSnapshotData> structure;
};

} // namespace huxerui::detail

namespace huxerui::testing {
namespace {
std::shared_ptr<huxerui::detail::UiTestSession> Session(const std::weak_ptr<huxerui::detail::UiTestSession>& weak) {
  auto session = weak.lock();
  if (!session) throw UiTestFailure("HuxerUI testing query outlived its session");
  session->Check();
  return session;
}
}

UiSelector::UiSelector(std::string description, std::function<bool(const UiNodeInfo&)> matches)
    : description_(std::move(description)), matches_(std::move(matches)) {}
UiSelector UiSelector::Text(std::string text) {
  auto description = "Text(" + detail::Quote(text) + ")";
  return {std::move(description), [text = std::move(text)](const auto& node) { return node.text == text; }};
}
UiSelector UiSelector::Value(std::string value) {
  return {"Value(<redacted>)", [value = std::move(value)](const auto& node) { return node.value == value; }};
}
UiSelector UiSelector::Kind(std::string type) {
  return {"Type(" + type + ")", [type](const auto& node) { return node.type == type; }};
}
UiSelector UiSelector::Key(std::string key) {
  auto description = "Key(" + detail::Quote(key) + ")";
  return {std::move(description), [key = UiKey(std::move(key))](const auto& node) { return node.key == key; }};
}
UiSelector UiSelector::Key(std::string_view key) { return Key(std::string(key)); }
UiSelector UiSelector::Key(const char* key) {
  if (!key) throw std::invalid_argument("HuxerUI testing key must not be null");
  return Key(std::string(key));
}
UiSelector UiSelector::Key(std::int64_t key) {
  return {"Key(signed " + std::to_string(key) + ")", [key = UiKey(key)](const auto& node) { return node.key == key; }};
}
UiSelector UiSelector::Key(std::uint64_t key) {
  return {"Key(unsigned " + std::to_string(key) + ")", [key = UiKey(key)](const auto& node) { return node.key == key; }};
}
UiSelector UiSelector::Enabled(bool value) {
  return {value ? "Enabled(true)" : "Enabled(false)", [value](const auto& n) { return n.enabled == value; }};
}
UiSelector UiSelector::Focused(bool value) {
  return {value ? "Focused(true)" : "Focused(false)", [value](const auto& n) { return n.focused == value; }};
}
UiSelector UiSelector::AllOf(std::initializer_list<UiSelector> values) {
  if (!values.size()) throw std::invalid_argument("HuxerUI testing conjunction must not be empty");
  std::string description = "AllOf(";
  for (const auto& value : values) { if (description != "AllOf(") description += ", "; description += value.description_; }
  return {description + ")", [values = std::vector<UiSelector>(values)](const auto& n) {
    return std::all_of(values.begin(), values.end(), [&](const auto& value) { return value.matches_(n); });
  }};
}

UiSemanticSelector::UiSemanticSelector(std::string description, std::function<bool(const SemanticNode&)> matches)
    : description_(std::move(description)), matches_(std::move(matches)) {}
UiSemanticSelector UiSemanticSelector::Identifier(std::string value) {
  auto description = "Identifier(" + detail::Quote(value) + ")";
  return {std::move(description), [value = std::move(value)](const auto& n) { return n.identifier == value; }};
}
UiSemanticSelector UiSemanticSelector::Label(std::string value) {
  auto description = "Label(" + detail::Quote(value) + ")";
  return {std::move(description), [value = std::move(value)](const auto& n) { return n.label == value; }};
}
UiSemanticSelector UiSemanticSelector::Value(std::string value) {
  return {"Value(<redacted>)", [value = std::move(value)](const auto& n) { return !n.secure && n.value == value; }};
}
UiSemanticSelector UiSemanticSelector::Role(SemanticRole value) {
  return {"Role(" + std::to_string(static_cast<int>(value)) + ")", [value](const auto& n) { return n.role == value; }};
}
UiSemanticSelector UiSemanticSelector::Enabled(bool value) {
  return {value ? "Enabled(true)" : "Enabled(false)", [value](const auto& n) { return n.enabled == value; }};
}
UiSemanticSelector UiSemanticSelector::Focused(bool value) {
  return {value ? "Focused(true)" : "Focused(false)", [value](const auto& n) { return n.focused == value; }};
}
UiSemanticSelector UiSemanticSelector::Selected(bool value) {
  return {value ? "Selected(true)" : "Selected(false)", [value](const auto& n) { return n.selected == value; }};
}
UiSemanticSelector UiSemanticSelector::Checked(SemanticCheckedState value) {
  return {"Checked(" + std::to_string(static_cast<int>(value)) + ")", [value](const auto& n) { return n.checked == value; }};
}
UiSemanticSelector UiSemanticSelector::AllOf(std::initializer_list<UiSemanticSelector> values) {
  if (!values.size()) throw std::invalid_argument("HuxerUI testing conjunction must not be empty");
  std::string description = "AllOf(";
  for (const auto& value : values) { if (description != "AllOf(") description += ", "; description += value.description_; }
  return {description + ")", [values = std::vector<UiSemanticSelector>(values)](const auto& n) {
    return std::all_of(values.begin(), values.end(), [&](const auto& value) { return value.matches_(n); });
  }};
}

UiNodeQuery::UiNodeQuery(std::weak_ptr<huxerui::detail::UiTestSession> session, std::vector<UiSelector> selectors)
    : session_(std::move(session)), selectors_(std::move(selectors)) {}
UiNodeQuery UiNodeQuery::Find(UiSelector selector) const {
  auto chain = selectors_;
  chain.push_back(std::move(selector));
  return {session_, std::move(chain)};
}
bool UiNodeQuery::Exists() const { return Count() != 0; }
std::size_t UiNodeQuery::Count() const { return Session(session_)->Resolve(selectors_).size(); }
UiNodeInfo UiNodeQuery::One() const { return Session(session_)->One(selectors_).info; }
std::vector<UiNodeInfo> UiNodeQuery::All() const {
  auto session = Session(session_);
  std::vector<UiNodeInfo> result;
  for (auto index : session->Resolve(selectors_)) result.push_back(session->nodes[index].info);
  return result;
}
void UiNodeQuery::Tap(UiPointerOptions options) const {
  auto session = Session(session_);
  session->Tap(session->TargetPoint(selectors_, "Tap"), options);
}
Point UiNodeQuery::ScrollBy(Point delta) const {
  auto session = Session(session_);
  session->Check(true);
  detail::ValidatePoint(delta);
  const Point point = session->TargetPoint(selectors_, "ScrollBy");
  const auto consumed = session->Mutate([&] {
    return session->runtime->HandleScrollInput({.position = point, .delta_x = delta.x, .delta_y = delta.y});
  });
  session->Pump(0);
  return consumed;
}
UiNodeQuery UiNodeQuery::ScrollUntil(UiSelector target, UiScrollOptions options) const {
  auto session = Session(session_);
  session->Check(true);
  detail::ValidatePoint(options.step);
  detail::ValidateDuration(options.interval.count());
  detail::ValidateDuration(session->adapter.time + options.interval.count() * options.maximum_steps);
  if (options.step == Point{} || options.interval.count() <= 0 || !options.maximum_steps)
    throw std::invalid_argument("HuxerUI testing ScrollUntil requires a nonzero delta, positive interval and step bound");
  const auto identity = session->One(selectors_, "ScrollUntil").identity;
  auto query = Find(std::move(target));
  for (std::size_t step = 0;; ++step) {
    if (session->One(selectors_, "ScrollUntil").identity != identity)
      throw UiTestFailure("HuxerUI testing ScrollUntil container was replaced");
    const auto matches = session->Resolve(query.selectors_, "ScrollUntil");
    if (matches.size() > 1) session->One(query.selectors_, "ScrollUntil");
    if (matches.size() == 1 && session->nodes[matches.front()].info.in_viewport) return query;
    if (step == options.maximum_steps)
      throw UiTestFailure("HuxerUI testing ScrollUntil exhausted " + std::to_string(step) +
          " wheel updates\nQuery: " + session->Chain(query.selectors_, query.selectors_.size() - 1));
    ScrollBy(options.step);
    session->Pump(options.interval.count());
  }
}
void UiNodeQuery::EnterText(std::string text) const {
  Session(session_)->Edit(selectors_, {
    .kind = TextInputCommandKind::CommitText, .target = {}, .selection_after = {}, .text = std::move(text),
  });
}
void UiNodeQuery::ReplaceText(std::string text) const {
  Session(session_)->Edit(selectors_, {
    .kind = TextInputCommandKind::CommitText, .target = {}, .selection_after = {}, .text = std::move(text),
  }, true);
}
void UiNodeQuery::SetSelection(TextSelection selection) const {
  Session(session_)->Edit(selectors_, {
    .kind = TextInputCommandKind::SetSelection, .target = {}, .selection_after = selection, .text = {},
  });
}
void UiNodeQuery::Submit() const {
  auto session = Session(session_);
  const auto id = session->RequireEditor(selectors_);
  const bool accepted = session->Mutate([&] {
    return session->runtime->PerformTextInputAction(id, session->adapter.input_action);
  });
  if (!accepted) throw UiTestFailure("HuxerUI testing text action was rejected");
  session->Pump(0);
}

UiSemanticQuery::UiSemanticQuery(std::weak_ptr<huxerui::detail::UiTestSession> session,
                                 std::vector<UiSemanticSelector> selectors)
    : session_(std::move(session)), selectors_(std::move(selectors)) {}
UiSemanticQuery UiSemanticQuery::Find(UiSemanticSelector selector) const {
  auto chain = selectors_;
  chain.push_back(std::move(selector));
  return {session_, std::move(chain)};
}
bool UiSemanticQuery::Exists() const { return Count() != 0; }
std::size_t UiSemanticQuery::Count() const { return Session(session_)->Resolve(selectors_).size(); }
SemanticNode UiSemanticQuery::One() const { return Session(session_)->One(selectors_); }
std::vector<SemanticNode> UiSemanticQuery::All() const {
  auto session = Session(session_);
  std::vector<SemanticNode> result;
  for (const auto* node : session->Resolve(selectors_)) result.push_back(*node);
  return result;
}
void UiSemanticQuery::PerformSemanticAction(const SemanticAction& action) const {
  auto session = Session(session_);
  const auto node = session->One(selectors_, "PerformSemanticAction");
  const bool accepted = session->Mutate([&] { return session->runtime->PerformSemanticAction(node.id, action); });
  if (!accepted) throw UiTestFailure("HuxerUI testing semantic action was rejected");
  session->Pump(0);
}

UiTest::UiTest(const Application& application, UiTestOptions options)
    : session_(std::make_shared<huxerui::detail::UiTestSession>(application, options)) {}
UiTest::~UiTest() = default;
UiNodeQuery UiTest::Find(UiSelector selector) const {
  session_->Check();
  return {session_, {std::move(selector)}};
}
UiSemanticQuery UiTest::FindSemantics(UiSemanticSelector selector) const {
  session_->Check();
  return {session_, {std::move(selector)}};
}
void UiTest::Pump(std::chrono::duration<double> duration) { session_->Pump(duration.count()); }
void UiTest::PumpUntil(const std::function<bool()>& predicate, UiPumpOptions options) {
  session_->Check(true);
  huxerui::detail::ValidatePumpOptions(options, Now());
  if (!predicate) throw std::invalid_argument("HuxerUI testing PumpUntil requires a predicate");
  const double end = Now() + options.timeout.count();
  std::size_t frames = 0;
  while (true) {
    session_->evaluating = true;
    bool done;
    try {
      done = predicate();
    } catch (...) {
      session_->evaluating = false;
      throw;
    }
    session_->evaluating = false;
    if (done) return;
    if (Now() >= end || frames++ >= options.maximum_frames)
      throw UiTestFailure("HuxerUI testing PumpUntil exhausted its virtual duration or frame budget");
    const double step = std::min(options.step.count(), end - Now());
    if (Now() + step == Now()) throw UiTestFailure("HuxerUI testing virtual step cannot advance the clock");
    Pump(std::chrono::duration<double>(step));
  }
}
void UiTest::PumpAndSettle(UiPumpOptions options) {
  session_->Check(true);
  detail::ValidatePumpOptions(options, Now());
  const double start = Now();
  const double end = start + options.timeout.count();
  std::size_t frames = 1;
  Pump();
  while (true) {
    std::size_t callbacks;
    {
      std::scoped_lock lock(session_->queue->mutex);
      callbacks = session_->queue->callbacks.size();
    }
    const auto deadline = session_->adapter.frame_deadline;
    if (!callbacks && !deadline) return;
    if (Now() >= end || frames >= options.maximum_frames) {
      std::ostringstream message;
      message.imbue(std::locale::classic());
      message << "HuxerUI testing PumpAndSettle exhausted its budget; elapsed=" << Now() - start
              << "; frames=" << frames << "; callbacks=" << callbacks << "; next_deadline=";
      if (deadline) message << *deadline; else message << "none";
      throw UiTestFailure(message.str());
    }
    const double next = callbacks ? Now() : std::min(end, std::max(Now() + options.step.count(), *deadline));
    if (!callbacks && next <= Now()) throw UiTestFailure("HuxerUI testing virtual step cannot advance the clock");
    Pump(std::chrono::duration<double>(next - Now()));
    ++frames;
  }
}
std::optional<TextInputState> UiTest::ActiveTextInput() const {
  session_->Check();
  return session_->adapter.input_state;
}
double UiTest::Now() const { session_->Check(); return session_->adapter.time; }
void UiTest::SetWindowMetrics(WindowMetrics metrics) {
  session_->Mutate([&] {
    session_->runtime->SetWindowMetrics(metrics);
    session_->metrics = metrics;
  });
}
void UiTest::UpdateResourceConfiguration(ResourceConfiguration configuration) {
  session_->Mutate([&] {
    session_->runtime->UpdateResourceConfiguration(configuration);
    session_->adapter.configuration = std::move(configuration);
  });
}
void UiTest::SendPointer(const PointerEvent& event) { session_->Pointer(event); }
Point UiTest::SendScroll(const ScrollInputEvent& event) {
  return session_->Mutate([&] { return session_->runtime->HandleScrollInput(event); });
}
bool UiTest::SendKey(const KeyEvent& event) {
  return session_->Mutate([&] { return session_->runtime->HandleKeyEvent(event); });
}
TextInputApplyResult UiTest::SendTextInput(const TextInputCommandBatch& batch) {
  return session_->Mutate([&] { return session_->runtime->HandleTextInputCommands(batch); });
}
void UiTest::TapAt(Point position, UiPointerOptions options) { session_->Tap(position, options); }
void UiTest::PressKey(Key key, KeyModifiers modifiers) {
  SendKey({.type = KeyEventType::Down, .key = key, .modifiers = modifiers});
  Pump();
  SendKey({.type = KeyEventType::Up, .key = key, .modifiers = modifiers});
  Pump();
}
void UiTest::Drag(Point start, Point end, UiDragOptions options) {
  session_->Check(true);
  huxerui::detail::ValidatePoint(start);
  huxerui::detail::ValidatePoint(end);
  huxerui::detail::ValidateDuration(options.duration.count());
  huxerui::detail::ValidateDuration(Now() + options.duration.count());
  if (!options.segments || options.segments > 10000)
    throw std::invalid_argument("HuxerUI testing drag requires between 1 and 10000 segments");
  PointerEvent event{.type = PointerEventType::Down, .pointer_id = options.pointer.pointer_id, .position = start,
      .device_kind = options.pointer.device_kind, .changed_button = PointerButton::Primary,
      .pressed_buttons = PointerButton::Primary};
  session_->Gesture(event, [&] {
    SendPointer(event);
    Pump();
    const double origin = Now();
    for (std::size_t i = 1; i <= options.segments; ++i) {
      const auto fraction = static_cast<double>(i) / static_cast<double>(options.segments);
      session_->adapter.time = origin + options.duration.count() * fraction;
      event.type = PointerEventType::Move;
      event.changed_button = PointerButton::None;
      event.position = {std::lerp(start.x, end.x, static_cast<float>(fraction)),
                        std::lerp(start.y, end.y, static_cast<float>(fraction))};
      SendPointer(event);
      Pump();
    }
    event.type = PointerEventType::Up;
    event.changed_button = PointerButton::Primary;
    event.pressed_buttons = PointerButton::None;
    SendPointer(event);
    Pump();
  });
}
UiSnapshot UiTest::CaptureSnapshot() const {
  session_->Check();
  return UiSnapshot(session_->structure);
}

} // namespace huxerui::testing
