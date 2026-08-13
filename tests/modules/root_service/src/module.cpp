#include <huxerui_test_module/module.h>

#include <memory>

#include <huxerui_test_module_resources.h>

namespace huxerui_test_module {

void Install(huxerui::RootContext& root) {
  const int value = raw::module_value_txt.Domain() == "huxerui_test_module" ? 42 : 0;
  root.Provide(std::make_shared<Service>(Service{value}));
}

} // namespace huxerui_test_module
