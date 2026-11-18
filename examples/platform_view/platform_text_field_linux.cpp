#include "platform_text_field.h"

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <utility>

#include <huxerui/linux/platform_registry.h>

namespace huxerui::example {
namespace {

struct TextFieldInstance {
  explicit TextFieldInstance(PlatformEventEmitter emitter)
      : entry(GTK_WIDGET(g_object_ref_sink(gtk_entry_new()))), events(std::move(emitter)) {}

  ~TextFieldInstance() {
    g_object_unref(entry);
  }

  GtkWidget* entry;
  PlatformEventEmitter events;
  gulong changed = 0;
};

void UpdateText(TextFieldInstance& instance, const PlatformTextFieldProperties& properties) {
  auto* editable = GTK_EDITABLE(instance.entry);
  if (properties.text != gtk_editable_get_text(editable)) {
    g_signal_handler_block(editable, instance.changed);
    gtk_editable_set_text(editable, properties.text.c_str());
    g_signal_handler_unblock(editable, instance.changed);
  }
}

} // namespace

void InstallPlatformTextField(ApplicationContext& root) {
  linux::PlatformViewFactory<PlatformTextFieldProperties, TextFieldInstance> factory{
      .create = [](GtkWindow*, const PlatformTextFieldProperties& properties, PlatformEventEmitter events) {
        auto instance = std::make_shared<TextFieldInstance>(std::move(events));
        auto* editable = GTK_EDITABLE(instance->entry);
        gtk_entry_set_placeholder_text(GTK_ENTRY(instance->entry), "Edit PlatformView text");
        gtk_editable_set_text(editable, properties.text.c_str());
        instance->changed = g_signal_connect(editable, "changed", G_CALLBACK(+[](GtkEditable* entry, gpointer data) {
          const auto& instance = *static_cast<TextFieldInstance*>(data);
          instance.events.Emit<PlatformTextFieldEvents::Changed>(std::string(gtk_editable_get_text(entry)));
        }), instance.get());
        return instance;
      },
      .view = [](const std::shared_ptr<TextFieldInstance>& instance) { return instance->entry; },
      .update = UpdateText,
      .dispose = [](TextFieldInstance& instance) { g_signal_handler_disconnect(instance.entry, instance.changed); },
  };
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type, std::move(factory));
}

} // namespace huxerui::example
