// The Preview: an application that shows the library's components while the library is developed.

export module app;

import std;
import huxerui;
import @LIBRARY_MODULE@;

using namespace huxerui;

View App() {
  return Column {
    Text("@LIBRARY_PROJECT_NAME@ preview", TextRole::Title),
    LabelledCounter("Count"),
    Badge("preview"),
  }.With(
      Padding(32.0F),
      Spacing(16.0F)
  );
}

const Application application{
    App,
    {
        .window = {
            .title = "@PROJECT_NAME@",
        },
    }
};
