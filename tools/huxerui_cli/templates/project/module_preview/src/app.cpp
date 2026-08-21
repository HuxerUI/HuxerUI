#include <huxerui/huxerui.h>
#include <@MODULE_TARGET_NAME@/@MODULE_TARGET_NAME@.h>

using namespace huxerui;

View App() {
  return Text("@MODULE_PROJECT_NAME@ preview");
}

const Application application{
    App,
    {
        .window = {
            .title = "@PROJECT_NAME@",
        },
        .root_hooks = {
            @MODULE_TARGET_NAME@::Install,
        },
    }
};
