#include <huxerui/huxerui.h>

#include <cstdio>

using namespace huxerui;

View App() {
  return Column {
    Text("mcpp + HuxerUI", TextRole::Title),
    Text("This small page is compiled by mcpp and linked directly to the HuxerUI static library."),
    Divider(),
    Text("The SDK CLI does not need to know what the application is. It only delegates the build to mcpp."),
    Row {
      Button("Say hello").OnClick([] { std::puts("Hello from the HuxerUI mcpp demo."); }),
    }.With(Spacing(12.0F)),
  }.With(
      Padding(32.0F),
      Spacing(16.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(Color::Rgb(248, 249, 252))
  );
}

const Application application{
    App,
    {
        .window = {
            .title = "mcpp HuxerUI Demo",
            .initial_size = {640.0F, 420.0F},
        },
    },
};

int main() {
  return RunApplication();
}
