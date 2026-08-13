package org.huxerui;

import android.content.Context;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Build;
import android.os.Bundle;
import android.text.TextUtils;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewParent;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;

import java.nio.BufferUnderflowException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;

final class HuxerUIAccessibilityProvider extends AccessibilityNodeProvider {
    private static final int SEMANTICS_MAGIC = 0x4D535848;
    private static final int SEMANTICS_VERSION = 1;
    private static final int NO_VIRTUAL_VIEW = Integer.MIN_VALUE;
    private static final int CUSTOM_ACTION_ID_START = 0x02000000;

    private static final int ROLE_GENERIC = 0;
    private static final int ROLE_TEXT = 1;
    private static final int ROLE_HEADING = 2;
    private static final int ROLE_IMAGE = 3;
    private static final int ROLE_BUTTON = 4;
    private static final int ROLE_LINK = 5;
    private static final int ROLE_CHECKBOX = 6;
    private static final int ROLE_RADIO_BUTTON = 7;
    private static final int ROLE_SWITCH = 8;
    private static final int ROLE_SLIDER = 9;
    private static final int ROLE_PROGRESS = 10;
    private static final int ROLE_TEXT_FIELD = 11;
    private static final int ROLE_SEARCH_FIELD = 12;
    private static final int ROLE_TAB = 13;
    private static final int ROLE_TAB_LIST = 14;
    private static final int ROLE_MENU = 15;
    private static final int ROLE_MENU_ITEM = 16;
    private static final int ROLE_DIALOG = 17;
    private static final int ROLE_NAVIGATION = 18;
    private static final int ROLE_LIST = 19;
    private static final int ROLE_LIST_ITEM = 20;
    private static final int ROLE_GRID = 21;
    private static final int ROLE_GRID_CELL = 22;
    private static final int ROLE_SCROLL_VIEW = 23;

    private static final int SEMANTIC_ACTIVATE = 0;
    private static final int SEMANTIC_FOCUS = 1;
    private static final int SEMANTIC_SET_TEXT = 2;
    private static final int SEMANTIC_SET_SELECTION = 3;
    private static final int SEMANTIC_SET_VALUE = 4;
    private static final int SEMANTIC_INCREMENT = 5;
    private static final int SEMANTIC_DECREMENT = 6;
    private static final int SEMANTIC_SCROLL = 7;
    private static final int SEMANTIC_SHOW_ON_SCREEN = 8;
    private static final int SEMANTIC_EXPAND = 9;
    private static final int SEMANTIC_COLLAPSE = 10;
    private static final int SEMANTIC_DISMISS = 11;
    private static final int SEMANTIC_CUSTOM = 12;

    private static final int AXIS_HORIZONTAL = 0;
    private static final int LIVE_REGION_POLITE = 1;
    private static final int LIVE_REGION_ASSERTIVE = 2;

    private final HuxerUIView view;
    private final AccessibilityManager manager;
    private final Matrix globalTransform = new Matrix();
    private final RectF transformedBounds = new RectF();
    private final Rect temporaryRect = new Rect();
    private final Map<CustomActionKey, Integer> customActionIds = new HashMap<>();
    private final SparseArray<CustomActionKey> customActions = new SparseArray<>();
    private Frame frame;
    private int accessibilityFocusedId = NO_VIRTUAL_VIEW;
    private int hoveredId = NO_VIRTUAL_VIEW;
    private int nextCustomActionId = CUSTOM_ACTION_ID_START;

    HuxerUIAccessibilityProvider(HuxerUIView view) {
        this.view = view;
        manager = (AccessibilityManager) view.getContext().getSystemService(Context.ACCESSIBILITY_SERVICE);
    }

    void commitFrame(byte[] encoded, boolean platformViewStructureChanged) {
        Frame next = Frame.decode(encoded);
        Frame previous = frame;
        frame = next;
        synchronizePlatformViews(next);
        Node accessibilityFocused = next.nodes.get(accessibilityFocusedId);
        if (accessibilityFocusedId != NO_VIRTUAL_VIEW
                && (accessibilityFocused == null || accessibilityFocused.platformViewIdentity != null)) {
            int staleId = accessibilityFocusedId;
            accessibilityFocusedId = NO_VIRTUAL_VIEW;
            sendEvent(staleId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
        }
        if (!isAccessibilityEnabled()) {
            return;
        }
        publishChanges(previous, next, platformViewStructureChanged);
    }

    void commitPlatformViews() {
        if (frame == null) {
            return;
        }
        synchronizePlatformViews(frame);
        if (isAccessibilityEnabled()) {
            sendContentChanged(AccessibilityNodeProvider.HOST_VIEW_ID, AccessibilityEvent.CONTENT_CHANGE_TYPE_SUBTREE);
        }
    }

    void reset() {
        view.resetPlatformViewAccessibility();
        frame = null;
        accessibilityFocusedId = NO_VIRTUAL_VIEW;
        hoveredId = NO_VIRTUAL_VIEW;
        customActionIds.clear();
        customActions.clear();
        nextCustomActionId = CUSTOM_ACTION_ID_START;
    }

    boolean dispatchHoverEvent(MotionEvent event) {
        if (frame == null || manager == null || !manager.isEnabled() || !manager.isTouchExplorationEnabled()) {
            return false;
        }
        int action = event.getActionMasked();
        if (action != MotionEvent.ACTION_HOVER_EXIT && view.platformViewAt(event.getX(), event.getY()) != 0L) {
            updateHoveredNode(NO_VIRTUAL_VIEW);
            return false;
        }
        if (action == MotionEvent.ACTION_HOVER_EXIT) {
            updateHoveredNode(NO_VIRTUAL_VIEW);
            return true;
        }
        if (action == MotionEvent.ACTION_HOVER_ENTER || action == MotionEvent.ACTION_HOVER_MOVE) {
            updateHoveredNode(hitTest(event.getX() / view.density(), event.getY() / view.density()));
            return true;
        }
        return false;
    }

    @Override
    public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualViewId) {
        Frame current = frame;
        if (virtualViewId == AccessibilityNodeProvider.HOST_VIEW_ID
                || (current != null && virtualViewId == current.root)) {
            return createHostNode();
        }
        Node node = current == null ? null : current.nodes.get(virtualViewId);
        return node == null || node.platformViewIdentity != null ? null : createVirtualNode(current, node);
    }

    @Override
    public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(String searched, int virtualViewId) {
        List<AccessibilityNodeInfo> result = new ArrayList<>();
        Frame current = frame;
        if (current == null || TextUtils.isEmpty(searched)) {
            return result;
        }
        String needle = searched.toLowerCase(Locale.ROOT);
        Node root = virtualViewId == AccessibilityNodeProvider.HOST_VIEW_ID ? current.nodes.get(current.root)
                                                                           : current.nodes.get(virtualViewId);
        if (root != null) {
            collectMatchingNodes(current, root, needle, result);
        }
        return result;
    }

    @Override
    public AccessibilityNodeInfo findFocus(int focus) {
        Frame current = frame;
        if (current == null) {
            return null;
        }
        if (focus == AccessibilityNodeInfo.FOCUS_ACCESSIBILITY) {
            return accessibilityFocusedId == NO_VIRTUAL_VIEW ? null
                                                              : createAccessibilityNodeInfo(accessibilityFocusedId);
        }
        if (focus == AccessibilityNodeInfo.FOCUS_INPUT) {
            for (Node node : current.orderedNodes) {
                if (node.focused && node.id != current.root && node.platformViewIdentity == null) {
                    return createVirtualNode(current, node);
                }
            }
        }
        return null;
    }

    @Override
    public boolean performAction(int virtualViewId, int action, Bundle arguments) {
        Frame current = frame;
        if (virtualViewId == AccessibilityNodeProvider.HOST_VIEW_ID
                || (current != null && virtualViewId == current.root)) {
            return view.performAccessibilityAction(action, arguments);
        }
        Node node = current == null ? null : current.nodes.get(virtualViewId);
        if (node == null || node.platformViewIdentity != null) {
            return false;
        }
        if (action == AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
            return requestAccessibilityFocus(node.id);
        }
        if (action == AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
            return clearAccessibilityFocus(node.id);
        }

        boolean handled = performRuntimeAction(node, action, arguments);
        if (handled && action == AccessibilityNodeInfo.ACTION_CLICK) {
            sendEvent(node.id, AccessibilityEvent.TYPE_VIEW_CLICKED);
        }
        return handled;
    }

    private AccessibilityNodeInfo createHostNode() {
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(view);
        view.onInitializeAccessibilityNodeInfo(info);
        info.setClassName(HuxerUIView.class.getName());
        info.setPackageName(view.getContext().getPackageName());
        // The native View owns keyboard input, but its synthetic semantic root is only a container for virtual nodes.
        info.setClickable(false);
        info.setFocusable(false);
        info.removeAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
        info.removeAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_FOCUS);
        info.removeAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLEAR_FOCUS);
        view.removePlatformViewAccessibilityChildren(info);
        Frame current = frame;
        Node root = current == null ? null : current.nodes.get(current.root);
        if (root != null) {
            for (int child : root.children) {
                addChild(current, info, child);
            }
        }
        return info;
    }

    private AccessibilityNodeInfo createVirtualNode(Frame current, Node node) {
        if (node.platformViewIdentity != null) {
            return null;
        }
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain();
        info.setPackageName(view.getContext().getPackageName());
        info.setSource(view, node.id);
        if (node.parent < 0 || node.parent == current.root) {
            info.setParent(view);
        } else {
            info.setParent(view, node.parent);
        }
        for (int child : node.children) {
            addChild(current, info, child);
        }
        info.setClassName(className(node.role));
        applyBounds(current, node, info);
        applyText(node, info);
        applyState(node, info);
        applyCollection(current, node, info);
        applyActions(node, info);
        return info;
    }

    private void addChild(Frame current, AccessibilityNodeInfo info, int childId) {
        Node child = current.nodes.get(childId);
        if (child == null) {
            return;
        }
        if (child.platformViewIdentity == null) {
            info.addChild(view, childId);
            return;
        }
        View platformChild = view.platformViewAccessibilityChild(child.platformViewIdentity);
        if (platformChild != null) {
            info.addChild(platformChild);
        }
    }

    private void synchronizePlatformViews(Frame current) {
        view.resetPlatformViewAccessibility();
        for (Node node : current.orderedNodes) {
            if (node.platformViewIdentity == null) {
                continue;
            }
            int parentId = node.parent < 0 || node.parent == current.root ? AccessibilityNodeProvider.HOST_VIEW_ID
                                                                         : node.parent;
            view.configurePlatformViewAccessibility(node.platformViewIdentity, parentId);
        }
    }

    private void applyBounds(Frame current, Node node, AccessibilityNodeInfo info) {
        float parentX = 0.0F;
        float parentY = 0.0F;
        if (node.parent >= 0 && node.parent != current.root) {
            Node parent = current.nodes.get(node.parent);
            if (parent != null) {
                parentX = parent.x;
                parentY = parent.y;
            }
        }
        float density = view.density();
        temporaryRect.set(Math.round((node.x - parentX) * density), Math.round((node.y - parentY) * density),
                Math.round((node.x + node.width - parentX) * density),
                Math.round((node.y + node.height - parentY) * density));
        info.setBoundsInParent(temporaryRect);

        transformedBounds.set(node.x * density, node.y * density, (node.x + node.width) * density,
                (node.y + node.height) * density);
        view.transformToScreen(globalTransform);
        globalTransform.mapRect(transformedBounds);
        temporaryRect.set(Math.round(transformedBounds.left), Math.round(transformedBounds.top),
                Math.round(transformedBounds.right), Math.round(transformedBounds.bottom));
        info.setBoundsInScreen(temporaryRect);
        info.setVisibleToUser(view.isShown() && !node.offscreen && node.width > 0.0F && node.height > 0.0F);
    }

    private void applyText(Node node, AccessibilityNodeInfo info) {
        boolean textRole = node.role == ROLE_TEXT || node.role == ROLE_HEADING;
        boolean editable = node.role == ROLE_TEXT_FIELD || node.role == ROLE_SEARCH_FIELD;
        if (editable) {
            if (!node.secure) {
                info.setText(node.value);
            }
            if (!TextUtils.isEmpty(node.label)) {
                info.setContentDescription(node.label);
            }
        } else if (textRole) {
            info.setText(!TextUtils.isEmpty(node.label) ? node.label : node.value);
        } else if (node.role == ROLE_IMAGE || node.role == ROLE_GENERIC) {
            info.setContentDescription(node.label);
        } else {
            info.setText(node.label);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && !TextUtils.isEmpty(node.placeholder)) {
            info.setHintText(node.placeholder);
            info.setShowingHintText(editable && TextUtils.isEmpty(node.value));
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && !TextUtils.isEmpty(node.hint)) {
            info.setTooltipText(node.hint);
        }
        if (!TextUtils.isEmpty(node.error)) {
            info.setError(node.error);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            String description = stateDescription(node);
            if (!TextUtils.isEmpty(description)) {
                info.setStateDescription(description);
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !TextUtils.isEmpty(node.identifier)) {
            info.setUniqueId(node.identifier);
        } else if (isResourceIdentifier(node.identifier)) {
            info.setViewIdResourceName(view.getContext().getPackageName() + ":id/" + node.identifier);
        }
    }

    private void applyState(Node node, AccessibilityNodeInfo info) {
        info.setEnabled(node.enabled);
        info.setFocusable(hasAction(node, SEMANTIC_FOCUS));
        info.setFocused(node.focused);
        info.setAccessibilityFocused(node.id == accessibilityFocusedId);
        info.setSelected(Boolean.TRUE.equals(node.selected));
        info.setCheckable(node.checked >= 0);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.BAKLAVA) {
            int checked = node.checked == 2 ? AccessibilityNodeInfo.CHECKED_STATE_PARTIAL
                                            : node.checked == 1 ? AccessibilityNodeInfo.CHECKED_STATE_TRUE
                                                                : AccessibilityNodeInfo.CHECKED_STATE_FALSE;
            info.setChecked(checked);
            info.setFieldRequired(Boolean.TRUE.equals(node.required));
            if (node.expanded != null) {
                info.setExpandedState(Boolean.TRUE.equals(node.expanded) ? AccessibilityNodeInfo.EXPANDED_STATE_FULL
                        : AccessibilityNodeInfo.EXPANDED_STATE_COLLAPSED);
            }
        } else {
            info.setChecked(node.checked == 1);
        }
        info.setClickable(hasAction(node, SEMANTIC_ACTIVATE));
        info.setEditable(hasAction(node, SEMANTIC_SET_TEXT));
        info.setMultiLine(node.multiline);
        info.setPassword(node.secure);
        info.setContentInvalid(Boolean.TRUE.equals(node.invalid));
        info.setScrollable(node.scroll != null && hasAction(node, SEMANTIC_SCROLL));
        info.setLiveRegion(node.liveRegion == LIVE_REGION_ASSERTIVE ? View.ACCESSIBILITY_LIVE_REGION_ASSERTIVE
                                                                    : node.liveRegion == LIVE_REGION_POLITE
                        ? View.ACCESSIBILITY_LIVE_REGION_POLITE
                        : View.ACCESSIBILITY_LIVE_REGION_NONE);
        if (node.textSelection != null && !node.secure) {
            info.setTextSelection(clampToInt(node.textSelection[0]), clampToInt(node.textSelection[1]));
        }
        if (node.range != null) {
            info.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT,
                    (float) node.range.minimum, (float) node.range.maximum, (float) node.range.current));
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            info.setHeading(node.role == ROLE_HEADING || node.headingLevel >= 0);
            if (node.role == ROLE_DIALOG && !TextUtils.isEmpty(node.label)) {
                info.setPaneTitle(node.label);
            }
        }
    }

    private void applyCollection(Frame current, Node node, AccessibilityNodeInfo info) {
        if (node.collection != null) {
            boolean vertical = node.role == ROLE_LIST || node.role == ROLE_MENU || node.role == ROLE_NAVIGATION;
            int rows = node.collection.rows >= 0 ? clampToInt(node.collection.rows)
                                                 : vertical && node.collection.items >= 0
                    ? clampToInt(node.collection.items)
                    : 0;
            int columns = node.collection.columns >= 0 ? clampToInt(node.collection.columns) : 1;
            int selectionMode = hasSingleSelectionChildren(current, node)
                    ? AccessibilityNodeInfo.CollectionInfo.SELECTION_MODE_SINGLE
                    : AccessibilityNodeInfo.CollectionInfo.SELECTION_MODE_NONE;
            info.setCollectionInfo(AccessibilityNodeInfo.CollectionInfo.obtain(rows, columns, false, selectionMode));
        }
        if (node.collectionItem != null) {
            int row = node.collectionItem.row >= 0 ? clampToInt(node.collectionItem.row)
                                                    : clampToInt(Math.max(0L, node.collectionItem.index));
            int column = node.collectionItem.column >= 0 ? clampToInt(node.collectionItem.column) : 0;
            info.setCollectionItemInfo(AccessibilityNodeInfo.CollectionItemInfo.obtain(row,
                    Math.max(1, clampToInt(node.collectionItem.rowSpan)), column,
                    Math.max(1, clampToInt(node.collectionItem.columnSpan)), node.role == ROLE_HEADING,
                    Boolean.TRUE.equals(node.selected)));
        }
    }

    private static boolean hasSingleSelectionChildren(Frame current, Node node) {
        if (node.role == ROLE_TAB_LIST || node.role == ROLE_NAVIGATION) {
            return true;
        }
        for (int childId : node.children) {
            Node child = current.nodes.get(childId);
            if (child != null && child.role == ROLE_RADIO_BUTTON) {
                return true;
            }
        }
        return false;
    }

    private void applyActions(Node node, AccessibilityNodeInfo info) {
        if (hasAction(node, SEMANTIC_ACTIVATE)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
            if (node.selected != null && !Boolean.TRUE.equals(node.selected)) {
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SELECT);
            }
        }
        if (hasAction(node, SEMANTIC_FOCUS)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_FOCUS);
        }
        if (hasAction(node, SEMANTIC_SET_TEXT)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_TEXT);
        }
        if (hasAction(node, SEMANTIC_SET_SELECTION)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_SELECTION);
        }
        if (hasAction(node, SEMANTIC_SET_VALUE) && Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS);
        }
        if (hasAction(node, SEMANTIC_SCROLL) || hasAction(node, SEMANTIC_INCREMENT)
                || hasAction(node, SEMANTIC_DECREMENT)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD);
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD);
            if (node.scroll != null) {
                if (node.scroll.axis == AXIS_HORIZONTAL) {
                    info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_LEFT);
                    info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_RIGHT);
                } else {
                    info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_UP);
                    info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_DOWN);
                }
            }
        }
        if (hasAction(node, SEMANTIC_SHOW_ON_SCREEN)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN);
        }
        if (hasAction(node, SEMANTIC_EXPAND)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_EXPAND);
        }
        if (hasAction(node, SEMANTIC_COLLAPSE)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_COLLAPSE);
        }
        if (hasAction(node, SEMANTIC_DISMISS)) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_DISMISS);
        }
        info.addAction(node.id == accessibilityFocusedId
                ? AccessibilityNodeInfo.AccessibilityAction.ACTION_CLEAR_ACCESSIBILITY_FOCUS
                : AccessibilityNodeInfo.AccessibilityAction.ACTION_ACCESSIBILITY_FOCUS);
        for (CustomAction custom : node.customActions) {
            int androidId = customActionId(node.id, custom.id);
            info.addAction(new AccessibilityNodeInfo.AccessibilityAction(androidId, custom.label));
        }
    }

    private boolean performRuntimeAction(Node node, int action, Bundle arguments) {
        if (action == AccessibilityNodeInfo.ACTION_CLICK && hasAction(node, SEMANTIC_ACTIVATE)) {
            return perform(node.id, SEMANTIC_ACTIVATE, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.ACTION_SELECT && hasAction(node, SEMANTIC_ACTIVATE)
                && node.selected != null) {
            return Boolean.TRUE.equals(node.selected)
                    || perform(node.id, SEMANTIC_ACTIVATE, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.ACTION_FOCUS && hasAction(node, SEMANTIC_FOCUS)) {
            return perform(node.id, SEMANTIC_FOCUS, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.ACTION_SET_TEXT && hasAction(node, SEMANTIC_SET_TEXT)) {
            CharSequence value = arguments == null
                    ? null
                    : arguments.getCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE);
            return value != null
                    && perform(node.id, SEMANTIC_SET_TEXT, value.toString(), 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.ACTION_SET_SELECTION && hasAction(node, SEMANTIC_SET_SELECTION)) {
            if (arguments == null) {
                return false;
            }
            int start = arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, -1);
            int end = arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, -1);
            return start >= 0 && end >= 0
                    && perform(node.id, SEMANTIC_SET_SELECTION, null, start, end, 0.0, 0.0F, 0.0F, 0L);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                && action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS.getId()
                && hasAction(node, SEMANTIC_SET_VALUE) && arguments != null) {
            float value = arguments.getFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE);
            return perform(node.id, SEMANTIC_SET_VALUE, null, 0, 0, value, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN.getId()
                && hasAction(node, SEMANTIC_SHOW_ON_SCREEN)) {
            return perform(node.id, SEMANTIC_SHOW_ON_SCREEN, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.ACTION_EXPAND && hasAction(node, SEMANTIC_EXPAND)) {
            return perform(node.id, SEMANTIC_EXPAND, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.ACTION_COLLAPSE && hasAction(node, SEMANTIC_COLLAPSE)) {
            return perform(node.id, SEMANTIC_COLLAPSE, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (action == AccessibilityNodeInfo.ACTION_DISMISS && hasAction(node, SEMANTIC_DISMISS)) {
            return perform(node.id, SEMANTIC_DISMISS, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (isScrollAction(action)) {
            return performScroll(node, action);
        }
        CustomActionKey custom = customActions.get(action);
        return custom != null && custom.nodeId == node.id
                && perform(node.id, SEMANTIC_CUSTOM, null, 0, 0, 0.0, 0.0F, 0.0F, custom.actionId);
    }

    private boolean performScroll(Node node, int action) {
        boolean forward = action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_RIGHT.getId()
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_DOWN.getId();
        if (node.scroll == null) {
            int kind = forward ? SEMANTIC_INCREMENT : SEMANTIC_DECREMENT;
            return hasAction(node, kind) && perform(node.id, kind, null, 0, 0, 0.0, 0.0F, 0.0F, 0L);
        }
        if (!hasAction(node, SEMANTIC_SCROLL)) {
            return false;
        }
        boolean horizontalAction = action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_LEFT.getId()
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_RIGHT.getId();
        boolean verticalAction = action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_UP.getId()
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_DOWN.getId();
        if ((node.scroll.axis == AXIS_HORIZONTAL && verticalAction)
                || (node.scroll.axis != AXIS_HORIZONTAL && horizontalAction)) {
            return false;
        }
        float delta = Math.max(48.0F, node.scroll.viewport * 0.8F) * (forward ? 1.0F : -1.0F);
        float x = node.scroll.axis == AXIS_HORIZONTAL ? delta : 0.0F;
        float y = node.scroll.axis == AXIS_HORIZONTAL ? 0.0F : delta;
        return perform(node.id, SEMANTIC_SCROLL, null, 0, 0, 0.0, x, y, 0L);
    }

    private boolean perform(int nodeId, int kind, String text, long argument0, long argument1, double number, float x,
            float y, long customId) {
        return view.performSemanticAction(nodeId, kind, text, argument0, argument1, number, x, y, customId);
    }

    private boolean requestAccessibilityFocus(int nodeId) {
        if (!isAccessibilityEnabled() || accessibilityFocusedId == nodeId) {
            return accessibilityFocusedId == nodeId;
        }
        if (accessibilityFocusedId != NO_VIRTUAL_VIEW) {
            int previous = accessibilityFocusedId;
            accessibilityFocusedId = NO_VIRTUAL_VIEW;
            sendEvent(previous, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
        }
        accessibilityFocusedId = nodeId;
        sendEvent(nodeId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED);
        view.invalidate();
        return true;
    }

    private boolean clearAccessibilityFocus(int nodeId) {
        if (accessibilityFocusedId != nodeId) {
            return false;
        }
        accessibilityFocusedId = NO_VIRTUAL_VIEW;
        sendEvent(nodeId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
        view.invalidate();
        return true;
    }

    private void updateHoveredNode(int nodeId) {
        if (hoveredId == nodeId) {
            return;
        }
        if (hoveredId != NO_VIRTUAL_VIEW) {
            sendEvent(hoveredId, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
        }
        hoveredId = nodeId;
        if (hoveredId != NO_VIRTUAL_VIEW) {
            sendEvent(hoveredId, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
        }
    }

    private int hitTest(float x, float y) {
        Frame current = frame;
        if (current == null) {
            return NO_VIRTUAL_VIEW;
        }
        for (int index = current.orderedNodes.length - 1; index >= 0; --index) {
            Node node = current.orderedNodes[index];
            if (node.id != current.root && node.platformViewIdentity == null && !node.offscreen && x >= node.x
                    && x <= node.x + node.width && y >= node.y && y <= node.y + node.height) {
                return node.id;
            }
        }
        return NO_VIRTUAL_VIEW;
    }

    private void collectMatchingNodes(
            Frame current, Node node, String needle, List<AccessibilityNodeInfo> result) {
        if (node.platformViewIdentity != null) {
            return;
        }
        if (node.id != current.root && (contains(node.label, needle) || contains(node.value, needle)
                || contains(node.hint, needle) || contains(node.stateDescription, needle))) {
            AccessibilityNodeInfo info = createVirtualNode(current, node);
            if (info != null) {
                result.add(info);
            }
        }
        for (int childId : node.children) {
            Node child = current.nodes.get(childId);
            if (child != null) {
                collectMatchingNodes(current, child, needle, result);
            }
        }
    }

    private void publishChanges(Frame previous, Frame next, boolean platformViewStructureChanged) {
        if (previous == null || platformViewStructureChanged || structureChanged(previous, next)) {
            sendContentChanged(AccessibilityNodeProvider.HOST_VIEW_ID, AccessibilityEvent.CONTENT_CHANGE_TYPE_SUBTREE);
        }
        for (Node current : next.orderedNodes) {
            Node before = previous == null ? null : previous.nodes.get(current.id);
            if (current.id == next.root || current.platformViewIdentity != null) {
                continue;
            }
            if (before == null) {
                if (current.focused) {
                    sendEvent(current.id, AccessibilityEvent.TYPE_VIEW_FOCUSED);
                }
                if (Boolean.TRUE.equals(current.selected)) {
                    sendEvent(current.id, AccessibilityEvent.TYPE_VIEW_SELECTED);
                }
                if (current.role == ROLE_DIALOG) {
                    sendEvent(current.id, AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED);
                }
                if (current.liveRegion != 0) {
                    sendContentChanged(current.id, AccessibilityEvent.CONTENT_CHANGE_TYPE_TEXT);
                }
                continue;
            }
            if (!before.focused && current.focused) {
                sendEvent(current.id, AccessibilityEvent.TYPE_VIEW_FOCUSED);
            }
            if (!Objects.equals(before.selected, current.selected) && Boolean.TRUE.equals(current.selected)) {
                sendEvent(current.id, AccessibilityEvent.TYPE_VIEW_SELECTED);
            }
            if (isEditable(current) && !Objects.equals(before.value, current.value)) {
                sendTextChanged(current, before.value);
            }
            if (!Arrays.equals(before.textSelection, current.textSelection) && current.textSelection != null) {
                sendEvent(current.id, AccessibilityEvent.TYPE_VIEW_TEXT_SELECTION_CHANGED);
            }
            if (before.scroll != null && current.scroll != null && before.scroll.offset != current.scroll.offset) {
                sendEvent(current.id, AccessibilityEvent.TYPE_VIEW_SCROLLED);
            }
            if (stateChanged(before, current)) {
                int change = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                        ? AccessibilityEvent.CONTENT_CHANGE_TYPE_STATE_DESCRIPTION
                        : AccessibilityEvent.CONTENT_CHANGE_TYPE_UNDEFINED;
                sendContentChanged(current.id, change);
            }
            if (current.liveRegion != 0 && (!Objects.equals(before.label, current.label)
                    || !Objects.equals(before.value, current.value))) {
                sendContentChanged(current.id, AccessibilityEvent.CONTENT_CHANGE_TYPE_TEXT);
            }
            if (current.role == ROLE_DIALOG && before.role != ROLE_DIALOG) {
                sendEvent(current.id, AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED);
            }
        }
    }

    private void sendTextChanged(Node node, String previous) {
        AccessibilityEvent event = createEvent(node.id, AccessibilityEvent.TYPE_VIEW_TEXT_CHANGED);
        event.setBeforeText(previous);
        event.setFromIndex(0);
        event.setRemovedCount(previous.length());
        event.setAddedCount(node.value.length());
        requestSendEvent(event);
    }

    private void sendContentChanged(int nodeId, int changeType) {
        AccessibilityEvent event = createEvent(nodeId, AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
        event.setContentChangeTypes(changeType);
        requestSendEvent(event);
    }

    private void sendEvent(int nodeId, int type) {
        requestSendEvent(createEvent(nodeId, type));
    }

    private AccessibilityEvent createEvent(int nodeId, int type) {
        AccessibilityEvent event = AccessibilityEvent.obtain(type);
        event.setPackageName(view.getContext().getPackageName());
        if (nodeId == AccessibilityNodeProvider.HOST_VIEW_ID) {
            event.setSource(view);
            event.setClassName(HuxerUIView.class.getName());
            return event;
        }
        event.setSource(view, nodeId);
        Frame current = frame;
        Node node = current == null ? null : current.nodes.get(nodeId);
        if (node != null) {
            event.setClassName(className(node.role));
            event.setEnabled(node.enabled);
            event.setChecked(node.checked == 1);
            event.setPassword(node.secure);
            if (!TextUtils.isEmpty(node.label)) {
                event.getText().add(node.label);
            }
            if (!node.secure && !TextUtils.isEmpty(node.value) && !Objects.equals(node.label, node.value)) {
                event.getText().add(node.value);
            }
            if (node.textSelection != null) {
                event.setFromIndex(clampToInt(node.textSelection[0]));
                event.setToIndex(clampToInt(node.textSelection[1]));
                event.setItemCount(node.secure ? 0 : node.value.length());
            }
            if (node.scroll != null) {
                int offset = Math.round(node.scroll.offset);
                int maximum = Math.round(node.scroll.maximum);
                if (node.scroll.axis == AXIS_HORIZONTAL) {
                    event.setScrollX(offset);
                    event.setMaxScrollX(maximum);
                } else {
                    event.setScrollY(offset);
                    event.setMaxScrollY(maximum);
                }
            }
        }
        return event;
    }

    private void requestSendEvent(AccessibilityEvent event) {
        if (!isAccessibilityEnabled()) {
            return;
        }
        ViewParent parent = view.getParent();
        if (parent != null) {
            parent.requestSendAccessibilityEvent(view, event);
        }
    }

    private int customActionId(int nodeId, long actionId) {
        CustomActionKey key = new CustomActionKey(nodeId, actionId);
        Integer existing = customActionIds.get(key);
        if (existing != null) {
            return existing;
        }
        if (nextCustomActionId == Integer.MAX_VALUE) {
            throw new IllegalStateException("HuxerUI Android custom semantic action id range is exhausted");
        }
        int androidId = nextCustomActionId++;
        customActionIds.put(key, androidId);
        customActions.put(androidId, key);
        return androidId;
    }

    private boolean isAccessibilityEnabled() {
        return manager != null && manager.isEnabled();
    }

    private static boolean structureChanged(Frame previous, Frame current) {
        if (previous.root != current.root || previous.nodes.size() != current.nodes.size()) {
            return true;
        }
        for (int index = 0; index < current.nodes.size(); ++index) {
            Node node = current.nodes.valueAt(index);
            Node before = previous.nodes.get(node.id);
            if (before == null || before.parent != node.parent || !Arrays.equals(before.children, node.children)
                    || !Objects.equals(before.platformViewIdentity, node.platformViewIdentity)) {
                return true;
            }
        }
        return false;
    }

    private static boolean stateChanged(Node before, Node current) {
        return before.checked != current.checked || !Objects.equals(before.expanded, current.expanded)
                || !Objects.equals(before.busy, current.busy) || !Objects.equals(before.invalid, current.invalid)
                || !Objects.equals(before.stateDescription, current.stateDescription)
                || !Objects.equals(before.error, current.error) || !sameRange(before.range, current.range);
    }

    private static boolean sameRange(Range before, Range current) {
        return before == current || (before != null && current != null && before.minimum == current.minimum
                && before.maximum == current.maximum && before.current == current.current
                && Objects.equals(before.step, current.step));
    }

    private static boolean hasAction(Node node, int action) {
        return (node.actions & (1L << action)) != 0L;
    }

    private static boolean isScrollAction(int action) {
        return action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
                || action == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_LEFT.getId()
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_RIGHT.getId()
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_UP.getId()
                || action == AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_DOWN.getId();
    }

    private static boolean isEditable(Node node) {
        return node.role == ROLE_TEXT_FIELD || node.role == ROLE_SEARCH_FIELD;
    }

    private static String stateDescription(Node node) {
        if (!TextUtils.isEmpty(node.stateDescription)) {
            return node.stateDescription;
        }
        return !TextUtils.isEmpty(node.value) && !isEditable(node) ? node.value : null;
    }

    private static String className(int role) {
        switch (role) {
        case ROLE_TEXT:
        case ROLE_HEADING:
        case ROLE_LINK:
            return "android.widget.TextView";
        case ROLE_IMAGE:
            return "android.widget.ImageView";
        case ROLE_BUTTON:
            return "android.widget.Button";
        case ROLE_CHECKBOX:
            return "android.widget.CheckBox";
        case ROLE_RADIO_BUTTON:
            return "android.widget.RadioButton";
        case ROLE_SWITCH:
            return "android.widget.Switch";
        case ROLE_SLIDER:
            return "android.widget.SeekBar";
        case ROLE_PROGRESS:
            return "android.widget.ProgressBar";
        case ROLE_TEXT_FIELD:
        case ROLE_SEARCH_FIELD:
            return "android.widget.EditText";
        case ROLE_TAB:
            return "android.app.ActionBar$Tab";
        case ROLE_TAB_LIST:
            return "android.widget.TabWidget";
        case ROLE_MENU:
        case ROLE_LIST:
        case ROLE_NAVIGATION:
            return "android.widget.ListView";
        case ROLE_MENU_ITEM:
        case ROLE_LIST_ITEM:
            return "android.widget.TextView";
        case ROLE_DIALOG:
            return "android.app.Dialog";
        case ROLE_GRID:
            return "android.widget.GridView";
        case ROLE_GRID_CELL:
            return "android.view.View";
        case ROLE_SCROLL_VIEW:
            return "android.widget.ScrollView";
        case ROLE_GENERIC:
        default:
            return "android.view.ViewGroup";
        }
    }

    private static int clampToInt(long value) {
        return (int) Math.max(Integer.MIN_VALUE, Math.min(Integer.MAX_VALUE, value));
    }

    private static boolean contains(String value, String needle) {
        return !TextUtils.isEmpty(value) && value.toLowerCase(Locale.ROOT).contains(needle);
    }

    private static boolean isResourceIdentifier(String value) {
        if (TextUtils.isEmpty(value) || !(value.charAt(0) == '_' || value.charAt(0) >= 'a' && value.charAt(0) <= 'z')) {
            return false;
        }
        for (int index = 1; index < value.length(); ++index) {
            char character = value.charAt(index);
            if (!(character == '_' || character >= 'a' && character <= 'z'
                    || character >= '0' && character <= '9')) {
                return false;
            }
        }
        return true;
    }

    private static final class Frame {
        final long revision;
        final int root;
        final SparseArray<Node> nodes;
        final Node[] orderedNodes;

        Frame(long revision, int root, SparseArray<Node> nodes, Node[] orderedNodes) {
            this.revision = revision;
            this.root = root;
            this.nodes = nodes;
            this.orderedNodes = orderedNodes;
        }

        static Frame decode(byte[] encoded) {
            if (encoded == null) {
                throw new IllegalArgumentException("HuxerUI Android semantic frame is missing");
            }
            try {
                ByteBuffer buffer = ByteBuffer.wrap(encoded).order(ByteOrder.LITTLE_ENDIAN);
                if (buffer.getInt() != SEMANTICS_MAGIC || buffer.getInt() != SEMANTICS_VERSION) {
                    throw new IllegalArgumentException("HuxerUI Android semantic frame version is invalid");
                }
                long revision = buffer.getLong();
                int root = buffer.getInt();
                int count = readCount(buffer, "node");
                SparseArray<Node> nodes = new SparseArray<>(count);
                Node[] orderedNodes = new Node[count];
                for (int index = 0; index < count; ++index) {
                    Node node = Node.decode(buffer);
                    if (nodes.get(node.id) != null) {
                        throw new IllegalArgumentException(
                                "HuxerUI Android semantic frame contains a duplicate node id");
                    }
                    nodes.put(node.id, node);
                    orderedNodes[index] = node;
                }
                if (buffer.hasRemaining() || nodes.get(root) == null) {
                    throw new IllegalArgumentException("HuxerUI Android semantic frame structure is invalid");
                }
                return new Frame(revision, root, nodes, orderedNodes);
            } catch (BufferUnderflowException | NegativeArraySizeException exception) {
                throw new IllegalArgumentException("HuxerUI Android semantic frame is truncated", exception);
            }
        }
    }

    private static final class Node {
        int id;
        int parent;
        Long platformViewIdentity;
        int role;
        int[] children;
        long actions;
        boolean enabled;
        boolean focused;
        boolean multiline;
        boolean secure;
        boolean offscreen;
        String label;
        String value;
        String placeholder;
        String hint;
        String stateDescription;
        String error;
        String identifier;
        int checked;
        Boolean selected;
        Boolean expanded;
        Boolean busy;
        Boolean readOnly;
        Boolean required;
        Boolean invalid;
        int headingLevel;
        Range range;
        long[] textSelection;
        Scroll scroll;
        Collection collection;
        CollectionItem collectionItem;
        int liveRegion;
        float x;
        float y;
        float width;
        float height;
        CustomAction[] customActions;

        static Node decode(ByteBuffer buffer) {
            Node node = new Node();
            node.id = buffer.getInt();
            node.parent = buffer.getInt();
            node.platformViewIdentity = readPresent(buffer) ? buffer.getLong() : null;
            node.role = buffer.getInt();
            int childCount = readCount(buffer, "child");
            node.children = new int[childCount];
            for (int index = 0; index < childCount; ++index) {
                node.children[index] = buffer.getInt();
            }
            node.actions = buffer.getLong();
            int flags = buffer.get() & 0xFF;
            node.enabled = (flags & 1) != 0;
            node.focused = (flags & 2) != 0;
            node.multiline = (flags & 4) != 0;
            node.secure = (flags & 8) != 0;
            node.offscreen = (flags & 16) != 0;
            node.label = readString(buffer);
            node.value = readString(buffer);
            node.placeholder = readString(buffer);
            node.hint = readString(buffer);
            node.stateDescription = readString(buffer);
            node.error = readString(buffer);
            node.identifier = readString(buffer);
            node.checked = buffer.get();
            node.selected = readOptionalBoolean(buffer);
            node.expanded = readOptionalBoolean(buffer);
            node.busy = readOptionalBoolean(buffer);
            node.readOnly = readOptionalBoolean(buffer);
            node.required = readOptionalBoolean(buffer);
            node.invalid = readOptionalBoolean(buffer);
            node.headingLevel = buffer.getInt();
            if (readPresent(buffer)) {
                node.range = new Range(buffer.getDouble(), buffer.getDouble(), buffer.getDouble(),
                        readPresent(buffer) ? buffer.getDouble() : null);
            }
            if (readPresent(buffer)) {
                node.textSelection = new long[] {buffer.getLong(), buffer.getLong()};
            }
            if (readPresent(buffer)) {
                node.scroll = new Scroll(buffer.getInt(), buffer.getFloat(), buffer.getFloat(), buffer.getFloat(),
                        buffer.getFloat());
            }
            if (readPresent(buffer)) {
                node.collection = new Collection(buffer.getLong(), buffer.getLong(), buffer.getLong());
            }
            if (readPresent(buffer)) {
                node.collectionItem = new CollectionItem(buffer.getLong(), buffer.getLong(), buffer.getLong(),
                        buffer.getLong(), buffer.getLong());
            }
            node.liveRegion = buffer.getInt();
            node.x = buffer.getFloat();
            node.y = buffer.getFloat();
            node.width = buffer.getFloat();
            node.height = buffer.getFloat();
            int customCount = readCount(buffer, "custom action");
            node.customActions = new CustomAction[customCount];
            for (int index = 0; index < customCount; ++index) {
                node.customActions[index] = new CustomAction(buffer.getLong(), readString(buffer));
            }
            return node;
        }
    }

    private static final class Range {
        final double minimum;
        final double maximum;
        final double current;
        final Double step;

        Range(double minimum, double maximum, double current, Double step) {
            this.minimum = minimum;
            this.maximum = maximum;
            this.current = current;
            this.step = step;
        }
    }

    private static final class Scroll {
        final int axis;
        final float offset;
        final float maximum;
        final float viewport;
        final float content;

        Scroll(int axis, float offset, float maximum, float viewport, float content) {
            this.axis = axis;
            this.offset = offset;
            this.maximum = maximum;
            this.viewport = viewport;
            this.content = content;
        }
    }

    private static final class Collection {
        final long items;
        final long rows;
        final long columns;

        Collection(long items, long rows, long columns) {
            this.items = items;
            this.rows = rows;
            this.columns = columns;
        }
    }

    private static final class CollectionItem {
        final long index;
        final long row;
        final long column;
        final long rowSpan;
        final long columnSpan;

        CollectionItem(long index, long row, long column, long rowSpan, long columnSpan) {
            this.index = index;
            this.row = row;
            this.column = column;
            this.rowSpan = rowSpan;
            this.columnSpan = columnSpan;
        }
    }

    private static final class CustomAction {
        final long id;
        final String label;

        CustomAction(long id, String label) {
            this.id = id;
            this.label = label;
        }
    }

    private static final class CustomActionKey {
        final int nodeId;
        final long actionId;

        CustomActionKey(int nodeId, long actionId) {
            this.nodeId = nodeId;
            this.actionId = actionId;
        }

        @Override
        public boolean equals(Object value) {
            if (this == value) {
                return true;
            }
            if (!(value instanceof CustomActionKey)) {
                return false;
            }
            CustomActionKey other = (CustomActionKey) value;
            return nodeId == other.nodeId && actionId == other.actionId;
        }

        @Override
        public int hashCode() {
            return 31 * nodeId + Long.hashCode(actionId);
        }
    }

    private static int readCount(ByteBuffer buffer, String kind) {
        int count = buffer.getInt();
        if (count < 0 || count > buffer.remaining()) {
            throw new IllegalArgumentException("HuxerUI Android semantic " + kind + " count is invalid");
        }
        return count;
    }

    private static String readString(ByteBuffer buffer) {
        int length = buffer.getInt();
        if (length < 0 || length > buffer.remaining()) {
            throw new IllegalArgumentException("HuxerUI Android semantic string length is invalid");
        }
        byte[] bytes = new byte[length];
        buffer.get(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static Boolean readOptionalBoolean(ByteBuffer buffer) {
        int value = buffer.get();
        return value < 0 ? null : value != 0;
    }

    private static boolean readPresent(ByteBuffer buffer) {
        return buffer.get() != 0;
    }
}
