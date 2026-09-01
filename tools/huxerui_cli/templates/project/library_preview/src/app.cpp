#include <huxerui/huxerui.h>
#include <@LIBRARY_PRIMARY_HEADER@>

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
            @LIBRARY_NAMESPACE@::Install,
        },
    }
};
