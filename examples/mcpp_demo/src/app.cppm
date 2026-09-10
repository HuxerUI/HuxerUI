// Everything the application is: its page, its composables, and the
// `Application` object whose constructor registers them.
//
// The global module fragment carries what this unit needs. huxerui.rules
// cannot force an include on a package with module units -- `-include`
// prepends before `module;`, which is ill-formed -- so a module unit writes
// them itself.
//
//   <typeinfo>                GCC's typeid check is per-TU, and UseState()
//                             instantiates typeid in the caller
//   huxerui_scope_prelude.h   HUXERUI_SCOPE and friends, for hand-written
//                             scopes; macros do not cross a module boundary.
//                             The rule puts this header's directory on the
//                             include path. Drop the line if you never write
//                             one by hand.

module;

#include <typeinfo>
#include <huxerui_scope_prelude.h>

export module app;

import huxerui;

using namespace huxerui;

// A HAND-WRITTEN scope, using the public macro directly and never touched by
// hcg. It compiles only because the prelude above is included -- the
// module-unit half of the guarantee the forced include gives a package that
// has no module units.
View Banner() {
  HUXERUI_SCOPE({ return Text("hand-written HUXERUI_SCOPE"); });
}

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Banner(),
    Row {
      Button("Count").OnClick([count] { count += 1; }),
      Text(count).With(FontSize(24.0F)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(8.0F));
}

View App() {
  return Column {
    Text("mcpp + HuxerUI", TextRole::Title),
    Divider(),
    Counter(),
  }.With(Padding(32.0F), Spacing(16.0F));
}

// Not exported: nothing needs to name it. Its constructor runs at static
// initialisation, which is what registers the application.
const Application application{
    App,
    {.window = {.title = "mcpp + HuxerUI", .initial_size = {560.0F, 360.0F}}},
};
