(() => {
  Module.huxeruiExamplePlatformTextFieldFactory = Object.freeze({
    create(properties, events) {
      const input = document.createElement("input");
      input.type = "text";
      input.placeholder = "Edit PlatformView text";
      input.value = properties.requireString();
      input.style.font = "16px system-ui, sans-serif";
      input.addEventListener("input", onInput);

      function onInput() {
        events.emit("changed", Module.HuxerUI.PlatformPayload.string(input.value));
      }

      return {
        element: input,
        update(nextProperties) {
          const value = nextProperties.requireString();
          if (input.value !== value) {
            input.value = value;
          }
        },
        dispose() {
          input.removeEventListener("input", onInput);
        },
      };
    },
  });
})();
