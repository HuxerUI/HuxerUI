// THE ONE CASE THAT STILL NEEDS A GLOBAL MODULE FRAGMENT.
//
// HUXERUI_SCOPE is a macro, and macros do not cross a module boundary:
// `import huxerui;` does not carry it, however much it carries otherwise. A
// unit that writes one by hand has to include the prelude, and an include
// belongs in a global module fragment -- which is what `module;` opens.
//
// huxerui.rules puts that header's directory on the include path. It cannot
// force the include, because `-include` prepends before `module;` and that is
// ill-formed.
//
// Marking the function [[huxerui::composable]] instead would need none of this:
// hcg injects the macro's EXPANSION, so generated code is macro-free. The macro
// is for the scope you write yourself.

module;

#include <huxerui_scope_prelude.h>

export module banner;

import std;
import huxerui;

using namespace huxerui;

export View Banner() {
  HUXERUI_SCOPE({ return Text("hand-written HUXERUI_SCOPE"); });
}
