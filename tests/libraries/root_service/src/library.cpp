#include <huxerui_test_library/library.h>

#include <memory>

#include <huxerui_test_library_resources.h>

namespace huxerui_test_library {

void Install(huxerui::RootContext& root) {
  const int value = raw::library_value_txt.Domain() == "huxerui_test_library" ? 42 : 0;
  root.Provide(std::make_shared<Service>(Service{value}));
}

} // namespace huxerui_test_library
