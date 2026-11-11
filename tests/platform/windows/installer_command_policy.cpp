#include <catch2/catch_amalgamated.hpp>

#include <huxerui/windows/installer.h>

#include "installer_command_policy.h"

namespace huxerui::test {

namespace {

using windows::InstallerAction;
using windows::InstallerProductState;

// Plain double-click launches are interactive: full display, install action.
constexpr BOOTSTRAPPER_ACTION kInteractiveAction = BOOTSTRAPPER_ACTION_INSTALL;
constexpr BOOTSTRAPPER_DISPLAY kInteractiveDisplay = BOOTSTRAPPER_DISPLAY_FULL;

} // namespace

TEST_CASE("installer display selects an interface only when useful", "[installer][windows]") {
  CHECK(detail::InstallerRunsHeadless(BOOTSTRAPPER_DISPLAY_EMBEDDED));
  CHECK(detail::InstallerRunsHeadless(BOOTSTRAPPER_DISPLAY_NONE));
  CHECK_FALSE(detail::InstallerRunsHeadless(BOOTSTRAPPER_DISPLAY_PASSIVE));
  CHECK_FALSE(detail::InstallerRunsHeadless(BOOTSTRAPPER_DISPLAY_FULL));
}

TEST_CASE("installer command preemption recognises unattended displays", "[installer][windows]") {
  CHECK_FALSE(detail::InstallerCommandPreemptsInteraction(kInteractiveDisplay));

  SECTION("embedded, quiet, and passive displays are preempted") {
    CHECK(detail::InstallerCommandPreemptsInteraction(BOOTSTRAPPER_DISPLAY_EMBEDDED));
    CHECK(detail::InstallerCommandPreemptsInteraction(BOOTSTRAPPER_DISPLAY_NONE));
    CHECK(detail::InstallerCommandPreemptsInteraction(BOOTSTRAPPER_DISPLAY_PASSIVE));
  }

  SECTION("an explicit action switch does not make a full-display run silent") {
    CHECK_FALSE(detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_UNINSTALL, kInteractiveDisplay).has_value());
    CHECK_FALSE(
        detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_UNSAFE_UNINSTALL, kInteractiveDisplay).has_value());
    CHECK_FALSE(detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_REPAIR, kInteractiveDisplay).has_value());
  }

  SECTION("other interactive actions are not preempted") {
    CHECK_FALSE(detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_MODIFY, kInteractiveDisplay).has_value());
    CHECK_FALSE(detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_HELP, kInteractiveDisplay).has_value());
    CHECK_FALSE(detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_UNKNOWN, kInteractiveDisplay).has_value());
  }
}

TEST_CASE("commanded action maps only executable commands", "[installer][windows]") {
  CHECK_FALSE(detail::InstallerCommandedAction(kInteractiveAction, kInteractiveDisplay).has_value());

  SECTION("embedded upgrade uninstall executes uninstall") {
    const auto action = detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_UNINSTALL, BOOTSTRAPPER_DISPLAY_EMBEDDED);
    REQUIRE(action.has_value());
    CHECK(*action == InstallerAction::Uninstall);
  }

  SECTION("unsafe uninstall maps to uninstall") {
    const auto action =
        detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_UNSAFE_UNINSTALL, BOOTSTRAPPER_DISPLAY_NONE);
    REQUIRE(action.has_value());
    CHECK(*action == InstallerAction::Uninstall);
  }

  SECTION("repair and quiet install map to their actions") {
    const auto repair =
        detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_REPAIR, BOOTSTRAPPER_DISPLAY_PASSIVE);
    REQUIRE(repair.has_value());
    CHECK(*repair == InstallerAction::Repair);

    const auto install =
        detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_INSTALL, BOOTSTRAPPER_DISPLAY_NONE);
    REQUIRE(install.has_value());
    CHECK(*install == InstallerAction::Install);
  }

  SECTION("commands without a session action produce no automatic action") {
    CHECK_FALSE(
        detail::InstallerCommandedAction(BOOTSTRAPPER_ACTION_LAYOUT, BOOTSTRAPPER_DISPLAY_EMBEDDED).has_value());
  }
}

TEST_CASE("superseded packages stay removable during commanded runs", "[installer][windows]") {
  CHECK(detail::InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE_PRESENT, false) ==
        InstallerProductState::SameVersion);
  CHECK(detail::InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE_ABSENT, false) ==
        InstallerProductState::Absent);
  CHECK(detail::InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE_UNKNOWN, false) ==
        InstallerProductState::Unknown);

  SECTION("interactive downgrade attempts keep the newer-version gate") {
    CHECK(detail::InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE_OBSOLETE, false) ==
          InstallerProductState::NewerVersion);
    CHECK(detail::InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE_SUPERSEDED, false) ==
          InstallerProductState::NewerVersion);
  }

  SECTION("commanded runs treat superseded packages as present so their registration is removed") {
    CHECK(detail::InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE_OBSOLETE, true) ==
          InstallerProductState::Present);
    CHECK(detail::InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE_SUPERSEDED, true) ==
          InstallerProductState::Present);
  }
}

TEST_CASE("engine prompts are only answered locally when a user is available", "[installer][windows]") {
  CHECK(detail::InstallerPromptAnswersLocally(false));
  CHECK_FALSE(detail::InstallerPromptAnswersLocally(true));
}

TEST_CASE("terminal events quit the engine when the window already exited", "[installer][windows]") {
  SECTION("interactive runs with a live window stay on the page") {
    CHECK(detail::InstallerTerminalActionFor(/*ui_closed=*/false, /*preempts=*/false) ==
          detail::InstallerTerminalAction::None);
  }

  SECTION("commanded runs with a live window close it so UiExited quits once") {
    CHECK(detail::InstallerTerminalActionFor(/*ui_closed=*/false, /*preempts=*/true) ==
          detail::InstallerTerminalAction::CloseWindow);
  }

  SECTION("headless runs quit without waiting for user interaction") {
    CHECK(detail::InstallerTerminalActionFor(/*ui_closed=*/false, /*preempts=*/true, /*headless=*/true) ==
          detail::InstallerTerminalAction::QuitEngine);
  }

  SECTION("an exited window after a failed plan must still quit the engine") {
    CHECK(detail::InstallerTerminalActionFor(/*ui_closed=*/true, /*preempts=*/false) ==
          detail::InstallerTerminalAction::QuitEngine);
    CHECK(detail::InstallerTerminalActionFor(/*ui_closed=*/true, /*preempts=*/true) ==
          detail::InstallerTerminalAction::QuitEngine);
  }
}

TEST_CASE("commanded uninstall stays meaningful while the bundle is registered", "[installer][windows]") {
  CHECK(detail::InstallerUninstallIsMeaningful(InstallerProductState::Present, /*bundle_registered=*/false));
  CHECK(detail::InstallerUninstallIsMeaningful(InstallerProductState::SameVersion, /*bundle_registered=*/false));
  CHECK_FALSE(detail::InstallerUninstallIsMeaningful(InstallerProductState::Absent, /*bundle_registered=*/false));
  CHECK_FALSE(detail::InstallerUninstallIsMeaningful(InstallerProductState::Unknown, /*bundle_registered=*/false));

  SECTION("a missing MSI with surviving bundle registration still uninstalls") {
    CHECK(detail::InstallerUninstallIsMeaningful(InstallerProductState::Absent, /*bundle_registered=*/true));
    CHECK(detail::InstallerUninstallIsMeaningful(InstallerProductState::Unknown, /*bundle_registered=*/true));
  }
}

TEST_CASE("installer versions distinguish upgrades, maintenance, and downgrades", "[installer][windows]") {
  CHECK(detail::InstallerCompareVersions("0.3.34", "0.3.35") == -1);
  CHECK(detail::InstallerCompareVersions("0.3.35", "0.3.35") == 0);
  CHECK(detail::InstallerCompareVersions("0.3.36", "0.3.35") == 1);
  CHECK(detail::InstallerCompareVersions("1", "1.0.0.0") == 0);
  CHECK(detail::InstallerCompareVersions("1.10", "1.9") == 1);
  for (const auto version : {"", "1.", "1..2", "1.2.3.4.5", "1.2-rc", "18446744073709551616"}) {
    CHECK_FALSE(detail::InstallerCompareVersions(version, "1").has_value());
    CHECK_FALSE(detail::InstallerCompareVersions("1", version).has_value());
  }
}

TEST_CASE("repair accepts both installed product states", "[installer][windows]") {
  CHECK(detail::InstallerRepairIsMeaningful(InstallerProductState::SameVersion));
  CHECK(detail::InstallerRepairIsMeaningful(InstallerProductState::Present));
  CHECK_FALSE(detail::InstallerRepairIsMeaningful(InstallerProductState::Absent));
  CHECK_FALSE(detail::InstallerRepairIsMeaningful(InstallerProductState::Unknown));
  CHECK_FALSE(detail::InstallerRepairIsMeaningful(InstallerProductState::NewerVersion));
}

} // namespace huxerui::test
