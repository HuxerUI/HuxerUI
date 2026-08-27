#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>

using namespace huxerui;

namespace {

struct StudioAsset {
  std::uint64_t id = 0;
  std::string title;
  std::string kind;
  bool ready = false;

  bool operator==(const StudioAsset&) const = default;
};

struct AssetTransfer {
  std::uint64_t id = 0;
  std::string title;
  bool ready = false;
};

void DeliverAsset(StateList<StudioAsset> inbox, StateList<StudioAsset> delivered, std::uint64_t id) {
  for (std::size_t index = 0; index < inbox.Size(); ++index) {
    if (inbox[index].id != id) {
      continue;
    }
    StudioAsset asset = inbox[index];
    inbox.Erase(index);
    delivered.PushBack(std::move(asset));
    return;
  }
}

std::string Rounded(float value, std::string suffix = {}) {
  return std::to_string(static_cast<int>(std::round(value))) + std::move(suffix);
}

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

View StatusDot(Color color) {
  return Canvas([color](PaintContext& paint, Size size) {
    paint.DrawCircle({size.width * 0.5F, size.height * 0.5F}, std::min(size.width, size.height) * 0.5F, color);
  }).With(Frame{7.0F, 7.0F});
}

View StatusLabel(std::string label, Color color, const ThemeSpec& theme) {
  return Row {
    StatusDot(color),
    Text(std::move(label), TextRole::Label).With(Foreground(color)),
  }.With(
      Spacing(theme.spacing.extra_small),
      CrossAlign(CrossAxisAlignment::Center)
  );
}

View Metric(std::string label, std::string value, const ThemeSpec& theme) {
  return Column {
    Text(std::move(value), TextRole::Title),
    Text(std::move(label), TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
  }.With(
      Spacing(theme.spacing.extra_small),
      CrossAlign(CrossAxisAlignment::Start)
  );
}

View LabCard(std::string eyebrow, std::string title, std::string description, View content, float width,
             const ThemeSpec& theme) {
  return Column {
    Column {
      Text(std::move(eyebrow), TextRole::Label).With(Foreground(theme.colors.primary)),
      Text(std::move(title), TextRole::Title),
      Text(std::move(description)).With(Foreground(theme.colors.on_surface_variant)),
    }.With(
        Spacing(theme.spacing.extra_small),
        CrossAlign(CrossAxisAlignment::Start)
    ),
    std::move(content),
  }.With(
      Frame{.width = width},
      Padding(theme.spacing.large),
      Spacing(theme.spacing.large),
      Background(theme.colors.surface),
      Border{WithAlpha(theme.colors.outline, 0.18F), 1.0F},
      CornerRadius(theme.shapes.large),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View StageGrid(Size size, const ThemeSpec& theme) {
  const ColorScheme colors = theme.colors;
  const float radius = theme.shapes.large;
  return Canvas([colors, radius](PaintContext& paint, Size canvas_size) {
    paint.DrawLinearGradient(
        {0.0F, 0.0F, canvas_size.width, canvas_size.height},
        LinearGradient{
            .start = {0.0F, 0.0F},
            .end = {1.0F, 1.0F},
            .stops = {
                {0.0F, colors.surface_container_high},
                {1.0F, colors.surface_container_low},
            },
        },
        CornerRadii{radius}
    );
    for (float x = 24.0F; x < canvas_size.width; x += 24.0F) {
      paint.DrawRect({x, 0.0F, 1.0F, canvas_size.height}, WithAlpha(colors.outline, 0.10F));
    }
    for (float y = 24.0F; y < canvas_size.height; y += 24.0F) {
      paint.DrawRect({0.0F, y, canvas_size.width, 1.0F}, WithAlpha(colors.outline, 0.10F));
    }
  }).With(Frame{size.width, size.height});
}

View QuickGestureLab(float width, bool stacked, State<std::uint32_t> clicks, State<std::uint32_t> multi_taps,
                     State<bool> long_press_active, State<std::string> long_press_status,
                     const ThemeSpec& theme) {
  View tap_surface = Column {
    Text(std::to_string(multi_taps.Get()), TextRole::Title).With(Foreground(theme.colors.on_secondary_container)),
    Text("MULTI-TAP", TextRole::Label).With(Foreground(theme.colors.on_secondary_container)),
    Text(std::to_string(clicks.Get()) + " clicks").With(Foreground(theme.colors.on_surface_variant)),
  }.With(
      Grow(),
      Frame{.height = 144.0F},
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.extra_small),
      Background(theme.colors.secondary_container),
      Foreground(theme.colors.on_secondary_container),
      CornerRadius(theme.shapes.large),
      MultiTapGesture{}
  )
      .OnClick([clicks] { clicks += 1; })
      .On<MultiTapEvents::Recognized>([multi_taps](const MultiTapEvent& event) {
        multi_taps = event.count;
      });

  View hold_surface = Column {
    Text(long_press_active.Get() ? "LIVE" : "HOLD", TextRole::Title)
        .With(Foreground(long_press_active.Get() ? theme.colors.on_primary : theme.colors.on_surface)),
    Text("LONG PRESS", TextRole::Label)
        .With(Foreground(long_press_active.Get() ? theme.colors.on_primary : theme.colors.on_surface)),
    Text(long_press_status.Get()).With(Foreground(
        long_press_active.Get() ? theme.colors.on_primary : theme.colors.on_surface_variant
    )),
  }.With(
      Grow(),
      Frame{.height = 144.0F},
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.extra_small),
      Background(long_press_active.Get() ? theme.colors.primary : theme.colors.surface_container_highest),
      Foreground(long_press_active.Get() ? theme.colors.on_primary : theme.colors.on_surface),
      CornerRadius(theme.shapes.large),
      LongPressGesture{}
  )
      .On<LongPressEvents::Started>([=](const LongPressEvent&) {
        long_press_active = true;
        long_press_status = "Release to complete";
      })
      .On<LongPressEvents::Ended>([=](const LongPressEvent&) {
        long_press_active = false;
        long_press_status = "Completed";
      })
      .On<LongPressEvents::Canceled>([=](const LongPressEvent&) {
        long_press_active = false;
        long_press_status = "Canceled";
      });

  View surfaces;
  if (stacked) {
    surfaces = Column {
      std::move(tap_surface),
      std::move(hold_surface),
    }.With(
        Spacing(theme.spacing.small),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  } else {
    surfaces = Row {
      std::move(tap_surface),
      std::move(hold_surface),
    }.With(
        Spacing(theme.spacing.small),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  }

  return LabCard(
      "DISCRETE INPUT",
      "Tap and hold",
      "Double-tap and hold share one pointer stream without changing Click behavior.",
      std::move(surfaces),
      width,
      theme
  );
}

View DragLab(float width, bool compact, State<Point> offset, State<Point> origin, State<Point> velocity,
             const ThemeSpec& theme) {
  const float stage_width = width - theme.spacing.large * 2.0F;
  const Point home{std::max(12.0F, (stage_width - 132.0F) * 0.5F), 78.0F};
  const auto reset = [=] {
    offset = home;
    velocity = {};
  };

  View handle = Column {
    Text("MOVE", TextRole::Label).With(Foreground(theme.colors.on_primary)),
    Text("Drag me").With(Foreground(theme.colors.on_primary)),
  }.With(
      Frame{132.0F, 68.0F},
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.extra_small),
      Background(theme.colors.primary),
      Foreground(theme.colors.on_primary),
      CornerRadius(theme.shapes.large),
      Shadow{
          .color = WithAlpha(theme.colors.scrim, 0.22F),
          .blur_radius = 16.0F,
          .spread = -4.0F,
      },
      Offset(offset.Get()),
      DragGesture{}
  )
      .On<DragEvents::Started>([=](const DragEvent&) { origin = offset.Get(); })
      .On<DragEvents::Changed>([=](const DragEvent& event) {
        const Point start = origin.Get();
        offset = {
            std::clamp(start.x + event.translation.x, 12.0F, std::max(12.0F, stage_width - 144.0F)),
            std::clamp(start.y + event.translation.y, 12.0F, 140.0F),
        };
        velocity = event.velocity;
      });

  View footer;
  if (compact) {
    footer = Column {
      Row {
        Metric("X position", Rounded(offset.Get().x), theme),
        Metric("Y position", Rounded(offset.Get().y), theme),
        Metric("Velocity", Rounded(std::hypot(velocity.Get().x, velocity.Get().y), " px/s"), theme),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Center)
      ),
      Button("Reset").OnClick(reset),
    }.With(
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  } else {
    footer = Row {
      Metric("X position", Rounded(offset.Get().x), theme),
      Metric("Y position", Rounded(offset.Get().y), theme),
      Metric("Velocity", Rounded(std::hypot(velocity.Get().x, velocity.Get().y), " px/s"), theme),
      Spacer(),
      Button("Reset").OnClick(reset),
    }.With(
        Spacing(theme.spacing.large),
        CrossAlign(CrossAxisAlignment::Center)
    );
  }

  return LabCard(
      "DIRECT MANIPULATION",
      "Pointer capture in motion",
      "Move outside the handle; capture keeps delivery continuous until release.",
      Column {
        Stack {
          StageGrid({stage_width, 220.0F}, theme),
          std::move(handle),
        }.With(
            Frame{stage_width, 220.0F},
            CornerRadius(theme.shapes.large),
            ClipChildren{}
        ),
        std::move(footer),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Stretch)
      ),
      width,
      theme
  );
}

View TransformLab(float width, bool compact, State<float> scale, State<float> rotation, State<std::uint32_t> pointers,
                  const ThemeSpec& theme) {
  const float stage_width = width - theme.spacing.large * 2.0F;
  const auto reset = [=] {
    scale = 1.0F;
    rotation = 0.0F;
  };
  const auto apply_transform = [=](const TransformEvent& event) {
    scale = scale.Get() * event.scale;
    rotation = rotation.Get() + event.rotation * 180.0F / std::numbers::pi_v<float>;
    pointers = event.pointer_count;
  };

  View poster = Column {
    Text("HUXER", TextRole::Title).With(Foreground(theme.colors.on_primary)),
    Text("MULTI-TOUCH STUDY", TextRole::Label).With(Foreground(theme.colors.on_primary)),
    Spacer(),
    Text("Pinch · Rotate").With(Foreground(theme.colors.on_primary)),
  }.With(
      Frame{190.0F, 142.0F},
      Padding(theme.spacing.medium),
      Background(LinearGradient{
          .start = {0.0F, 0.0F},
          .end = {1.0F, 1.0F},
          .stops = {
              {0.0F, theme.colors.primary},
              {1.0F, theme.colors.secondary},
          },
      }),
      Foreground(theme.colors.on_primary),
      CornerRadius(theme.shapes.large),
      Shadow{
          .color = WithAlpha(theme.colors.scrim, 0.24F),
          .blur_radius = 18.0F,
          .spread = -4.0F,
      },
      Scale(scale.Get()),
      Rotation(rotation.Get()),
      TransformGesture{}
  )
      .On<TransformEvents::Started>([=](const TransformEvent& event) { pointers = event.pointer_count; })
      .On<TransformEvents::Changed>(apply_transform)
      .On<TransformEvents::Ended>([=](const TransformEvent&) {
        pointers = 0;
      })
      .On<TransformEvents::Canceled>([=](const TransformEvent&) {
        pointers = 0;
      });

  View footer;
  if (compact) {
    footer = Column {
      Row {
        Metric("Scale", Rounded(scale.Get() * 100.0F, "%"), theme),
        Metric("Rotation", Rounded(rotation.Get(), "°"), theme),
        Metric("Pointers", std::to_string(pointers.Get()), theme),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Center)
      ),
      Button("Reset").OnClick(reset),
    }.With(
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  } else {
    footer = Row {
      Metric("Scale", Rounded(scale.Get() * 100.0F, "%"), theme),
      Metric("Rotation", Rounded(rotation.Get(), "°"), theme),
      Metric("Pointers", std::to_string(pointers.Get()), theme),
      Spacer(),
      Button("Reset").OnClick(reset),
    }.With(
        Spacing(theme.spacing.large),
        CrossAlign(CrossAxisAlignment::Center)
    );
  }

  return LabCard(
      "MULTI-POINTER",
      "Transform poster",
      "Pinch and rotate without limits; centroid translation is discarded.",
      Column {
        Stack {
          StageGrid({stage_width, 220.0F}, theme),
          std::move(poster),
        }.With(
            Frame{stage_width, 220.0F},
            Align(HorizontalAlignment::Center, VerticalAlignment::Center),
            CornerRadius(theme.shapes.large),
            ClipChildren{}
        ),
        std::move(footer),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Stretch)
      ),
      width,
      theme
  );
}

View AssetCard(const StudioAsset& asset, State<std::string> drop_status, const ThemeSpec& theme) {
  const AssetTransfer transfer{asset.id, asset.title, asset.ready};
  return Row {
    Column {
      Text(asset.title),
      Text(asset.kind, TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    }.With(
        Spacing(theme.spacing.extra_small),
        CrossAlign(CrossAxisAlignment::Start)
    ),
    Spacer(),
    StatusLabel(asset.ready ? "READY" : "DRAFT",
                asset.ready ? theme.colors.primary : theme.colors.on_surface_variant, theme),
  }.With(
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface_container),
      Border{WithAlpha(theme.colors.outline, 0.28F), 1.0F},
      CornerRadius(theme.shapes.medium),
      DragSource(
          transfer,
          [asset, theme] {
            return Row {
              Text(asset.title),
              StatusLabel("MOVING", theme.colors.primary, theme),
            }.With(
                Frame{.width = 260.0F},
                Padding(theme.spacing.medium),
                Spacing(theme.spacing.small),
                Background(theme.colors.surface),
                CornerRadius(theme.shapes.large),
                Shadow{
                    .color = WithAlpha(theme.colors.scrim, 0.24F),
                    .blur_radius = 24.0F,
                    .spread = -4.0F,
                },
                CrossAlign(CrossAxisAlignment::Center)
            );
          },
          DragGesture{.minimum_press_duration = std::chrono::duration<double>{0.18}}
      )
      )
      .On<DragSourceEvents::Started>([=](const DragEvent&) {
        drop_status = "Looking for a compatible destination";
      })
      .On<DragSourceEvents::Ended>([=](const DragDropResult& result) {
        if (!result.dropped) {
          drop_status = asset.ready ? "Drop outside the release queue" : "Draft assets are rejected";
        }
      })
      .On<DragSourceEvents::Canceled>([=](const DragEvent&) {
        drop_status = "Transfer canceled";
      })
      .Key(asset.id);
}

View InboxColumn(StateList<StudioAsset> inbox, State<std::string> drop_status, const ThemeSpec& theme) {
  return Column {
    Row {
      Text("Workspace", TextRole::Title),
      Spacer(),
      Text(std::to_string(inbox.Size()) + " ITEMS", TextRole::Label)
          .With(Foreground(theme.colors.on_surface_variant)),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Text("Press briefly, then drag a ready asset.").With(Foreground(theme.colors.on_surface_variant)),
    ForEach(inbox, [=](const StudioAsset& asset) {
      return AssetCard(asset, drop_status, theme);
    }),
  }.With(
      Grow(),
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface_container_low),
      Border{WithAlpha(theme.colors.outline, 0.18F), 1.0F},
      CornerRadius(theme.shapes.large),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View ReleaseColumn(StateList<StudioAsset> inbox, StateList<StudioAsset> delivered, State<bool> target_active,
                   State<std::string> drop_status, const ThemeSpec& theme) {
  View delivered_assets;
  if (delivered.Empty()) {
    delivered_assets = Text("Ready assets land here").With(
        Padding(theme.spacing.large),
        Foreground(theme.colors.on_surface_variant)
    );
  } else {
    delivered_assets = Column {
      ForEach(delivered, [theme](const StudioAsset& asset) {
        return Row {
          Text(asset.title),
          Spacer(),
          StatusLabel("SHIPPED", theme.colors.primary, theme),
        }.With(
            Padding(theme.spacing.medium),
            Background(theme.colors.surface),
            CornerRadius(theme.shapes.medium),
            CrossAlign(CrossAxisAlignment::Center)
        ).Key(asset.id);
      }),
    }.With(
        Spacing(theme.spacing.small),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  }

  return Column {
    Row {
      Text("Release queue", TextRole::Title),
      Spacer(),
      Text("READY ONLY", TextRole::Label)
          .With(Foreground(target_active.Get() ? theme.colors.primary : theme.colors.on_surface_variant)),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Text("The typed predicate rejects drafts before target events are emitted.")
        .With(Foreground(theme.colors.on_surface_variant)),
    std::move(delivered_assets),
  }.With(
      Grow(),
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(target_active.Get() ? WithAlpha(theme.colors.primary, 0.10F)
                                     : theme.colors.surface_container_low),
      Border{target_active.Get() ? theme.colors.primary : WithAlpha(theme.colors.outline, 0.18F),
             target_active.Get() ? 2.0F : 1.0F},
      CornerRadius(theme.shapes.large),
      DropTarget::Accepts<AssetTransfer>([](const AssetTransfer& transfer) { return transfer.ready; }),
      CrossAlign(CrossAxisAlignment::Stretch)
  )
      .On<DropEvents<AssetTransfer>::Entered>([=](const AssetTransfer& transfer, const DropEvent&) {
        target_active = true;
        drop_status = transfer.title + " is ready to release";
      })
      .On<DropEvents<AssetTransfer>::Exited>([=](const AssetTransfer& transfer, const DropEvent&) {
        target_active = false;
        drop_status = "Release queue left";
      })
      .On<DropEvents<AssetTransfer>::Dropped>([=](const AssetTransfer& transfer, const DropEvent&) {
        target_active = false;
        DeliverAsset(inbox, delivered, transfer.id);
        drop_status = transfer.title + " delivered";
      });
}

View TransferLab(float width, bool stacked, StateList<StudioAsset> inbox, StateList<StudioAsset> delivered,
                 State<bool> target_active, State<std::string> drop_status, const ThemeSpec& theme) {
  View board;
  if (stacked) {
    board = Column {
      InboxColumn(inbox, drop_status, theme),
      ReleaseColumn(inbox, delivered, target_active, drop_status, theme),
    }.With(
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  } else {
    board = Row {
      InboxColumn(inbox, drop_status, theme),
      ReleaseColumn(inbox, delivered, target_active, drop_status, theme),
    }.With(
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  }

  View status;
  if (stacked) {
    status = Column {
      StatusLabel(target_active.Get() ? "TARGET ACTIVE" : "WAITING",
                  target_active.Get() ? theme.colors.primary : theme.colors.on_surface_variant, theme),
      Text(drop_status.Get()).With(Foreground(theme.colors.on_surface_variant)),
    }.With(
        Spacing(theme.spacing.extra_small),
        CrossAlign(CrossAxisAlignment::Start)
    );
  } else {
    status = Row {
      StatusLabel(target_active.Get() ? "TARGET ACTIVE" : "WAITING",
                  target_active.Get() ? theme.colors.primary : theme.colors.on_surface_variant, theme),
      Text(drop_status.Get()).With(Foreground(theme.colors.on_surface_variant)),
    }.With(
        Spacing(theme.spacing.small),
        CrossAlign(CrossAxisAlignment::Center)
    );
  }

  return LabCard(
      "TYPED TRANSFER",
      "Release workflow",
      "Only payloads accepted by the typed destination can move into the release queue.",
      Column {
        std::move(status),
        std::move(board),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Stretch)
      ),
      width,
      theme
  );
}

[[huxerui::composable]]
View GestureStudio() {
  const ThemeSpec& theme = UseTheme();
  const ViewportClass viewport = UseViewportClass();
  const bool compact = viewport == ViewportClass::Compact;
  const bool expanded = viewport == ViewportClass::Expanded;
  const float panel_width = compact ? 328.0F : expanded ? 516.0F : 520.0F;
  const float board_width = compact ? 328.0F : expanded ? 1056.0F : 520.0F;

  auto clicks = UseState<std::uint32_t>(0);
  auto multi_taps = UseState<std::uint32_t>(0);
  auto long_press_active = UseState(false);
  auto long_press_status = UseState(std::string{"Hold to begin"});
  auto drag_offset = UseState(Point{74.0F, 78.0F});
  auto drag_origin = UseState(Point{});
  auto drag_velocity = UseState(Point{});
  auto transform_scale = UseState(1.0F);
  auto transform_rotation = UseState(0.0F);
  auto transform_pointers = UseState<std::uint32_t>(0);
  auto inbox = UseStateList<StudioAsset>({
      {1, "Motion study.fig", "Design file", true},
      {2, "Launch notes.md", "Document", true},
      {3, "Concept draft", "Unreviewed", false},
  });
  auto delivered = UseStateList<StudioAsset>();
  auto target_active = UseState(false);
  auto drop_status = UseState(std::string{"Drag a ready asset into the release queue"});

  View gesture_labs;
  if (expanded) {
    gesture_labs = Column {
      Row {
        DragLab(panel_width, false, drag_offset, drag_origin, drag_velocity, theme),
        TransformLab(panel_width, false, transform_scale, transform_rotation, transform_pointers, theme),
      }.With(
          Spacing(theme.spacing.large),
          CrossAlign(CrossAxisAlignment::Start)
      ),
      QuickGestureLab(board_width, false, clicks, multi_taps, long_press_active, long_press_status, theme),
    }.With(
        Spacing(theme.spacing.large),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  } else {
    gesture_labs = Column {
      QuickGestureLab(panel_width, compact, clicks, multi_taps, long_press_active, long_press_status, theme),
      DragLab(panel_width, compact, drag_offset, drag_origin, drag_velocity, theme),
      TransformLab(panel_width, compact, transform_scale, transform_rotation, transform_pointers, theme),
    }.With(
        Spacing(theme.spacing.large),
        CrossAlign(CrossAxisAlignment::Stretch)
    );
  }

  return ScrollView {
    Column {
      std::move(gesture_labs),
      TransferLab(board_width, !expanded, inbox, delivered, target_active, drop_status, theme),
    }.With(
        Padding(compact ? theme.spacing.medium : theme.spacing.extra_large),
        Spacing(theme.spacing.large),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(Background(theme.colors.surface_container_low));
}

} // namespace

View App() {
  return MaterialTheme {
    GestureStudio(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Gesture Studio",
            .initial_size = {1180.0F, 820.0F},
        },
    }
};
