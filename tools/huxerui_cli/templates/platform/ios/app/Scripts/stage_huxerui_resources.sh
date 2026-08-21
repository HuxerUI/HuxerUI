set -eu

HUXERUI_RESOURCE_SOURCE="$HUXERUI_CORE_BUILD_DIR/huxerui-ios/@TARGET_NAME@/resources/package"
HUXERUI_RESOURCE_DESTINATION="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/HuxerUI"
if [ ! -f "$HUXERUI_RESOURCE_SOURCE/huxerui/resources.bin" ]; then
  echo "error: HuxerUI resource package is missing: $HUXERUI_RESOURCE_SOURCE" >&2
  exit 1
fi

cmake -E remove_directory "$HUXERUI_RESOURCE_DESTINATION"
cmake -E make_directory "$HUXERUI_RESOURCE_DESTINATION"
cmake -E copy_directory "$HUXERUI_RESOURCE_SOURCE" "$HUXERUI_RESOURCE_DESTINATION"

if [ -n "${HUXERUI_INTEGRATION_PLAN:-}" ]; then
  mkdir -p "$(dirname "$HUXERUI_INTEGRATION_PLAN")"
  {
    printf '{\n'
    printf '  "schema": 1,\n'
    printf '  "target": "%s",\n' "@TARGET_NAME@"
    printf '  "platform": "ios",\n'
    printf '  "artifact": "%s",\n' "$TARGET_BUILD_DIR/$EXECUTABLE_PATH"
    printf '  "bundle": "%s",\n' "$TARGET_BUILD_DIR/$WRAPPER_NAME"
    printf '  "bundleIdentifier": "%s"\n' "$PRODUCT_BUNDLE_IDENTIFIER"
    printf '}\n'
  } > "$HUXERUI_INTEGRATION_PLAN"
fi
