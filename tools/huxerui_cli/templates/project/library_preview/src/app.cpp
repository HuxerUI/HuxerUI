#include <huxerui/huxerui.h>
#include <@LIBRARY_TARGET_NAME@/@LIBRARY_TARGET_NAME@.h>

using namespace huxerui;

View App() {
  return Text("@LIBRARY_PROJECT_NAME@ preview");
}

const Application application{
    App,
    {
        .window = {
            .title = "@PROJECT_NAME@",
        },
        .root_hooks = {
            @LIBRARY_TARGET_NAME@::Install,
        },
    }
};
