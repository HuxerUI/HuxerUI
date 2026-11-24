import { Configuration, ConfigurationConstant, UIAbility, Want, wantConstant } from '@kit.AbilityKit';
import { display, KeyboardAvoidMode, NodeContent, UIContext, window } from '@kit.ArkUI';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { i18n } from '@kit.LocalizationKit';
import { HuxerUIActivation, HuxerUIApplicationModule, HuxerUIWindowStageHost } from './HuxerUIApplicationModule';
import { HuxerUIBackgroundTasks } from './HuxerUIBackgroundTasks';
import { HuxerUIFilePicker, withGrantedFiles } from './HuxerUIFilePicker';
import { HuxerUIHttp } from './HuxerUIHttp';
import { HuxerUIPlatformExtensions } from './HuxerUIPlatform';
import { HuxerUISystemServices, notificationActivation } from './HuxerUISystemServices';
import { HuxerUISystemTasks } from './HuxerUISystemTasks';
import { HuxerUITextInput } from './HuxerUITextInput';

let applicationModule: HuxerUIApplicationModule | undefined;
let applicationWindows: HuxerUIWindows | undefined;
let nextStageId = 0;
let systemDark = false;

const systemAvoidAreaTypes: window.AvoidAreaType[] = [
  window.AvoidAreaType.TYPE_SYSTEM,
  window.AvoidAreaType.TYPE_CUTOUT,
  window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR,
];

function displayScale(): number {
  try {
    return display.getDefaultDisplaySync().densityPixels;
  } catch (_) {
    return 1;
  }
}

async function setStatusBarBrightness(mainWindow: window.Window, light: boolean): Promise<void> {
  try {
    await mainWindow.setWindowSystemBarProperties({
      statusBarColor: '#00000000',
      statusBarContentColor: light ? '#FFFFFFFF' : '#FF000000',
    });
  } catch (error) {
    console.warn(`HuxerUI could not set the status bar appearance: ${error}`);
  }
}

function isFileUri(uri: string): boolean {
  const scheme = uri.substring(0, uri.indexOf(':')).toLowerCase();
  return scheme === 'file' || scheme === 'datashare';
}

function withActivation(want: Want | undefined, receive: (activation?: HuxerUIActivation) => void): void {
  const notification = notificationActivation(want);
  if (notification) {
    receive({ notification: notification });
    return;
  }
  if (!want || want.action !== 'ohos.want.action.viewData') {
    receive();
    return;
  }
  const uri = want.uri;
  if (uri && !isFileUri(uri)) {
    receive({ url: uri });
    return;
  }
  const uris: string[] = uri ? [uri] : [];
  const streams = want.parameters?.[wantConstant.Params.PARAMS_STREAM];
  if (streams !== undefined) {
    if (!Array.isArray(streams)) {
      receive();
      return;
    }
    for (const entry of streams as string[]) {
      if (typeof entry !== 'string' || !isFileUri(entry)) {
        receive();
        return;
      }
      if (!uris.includes(entry)) {
        uris.push(entry);
      }
    }
  }
  const writable = ((want.flags ?? 0) & wantConstant.Flags.FLAG_AUTH_WRITE_URI_PERMISSION) !== 0;
  withGrantedFiles(uris, writable, files => receive(files ? { files: files } : undefined));
}

const windowStorageKey = 'huxeruiWindow';

class HuxerUIWindows implements HuxerUIWindowStageHost {
  readonly mainId: string = 'main-' + (++nextStageId).toString();
  private stage: window.WindowStage;
  private module: HuxerUIApplicationModule;
  private quit: () => Promise<void>;
  private windows: Map<string, window.Window> = new Map();
  private opening: Set<string> = new Set();
  private closing: Set<string> = new Set();

  constructor(stage: window.WindowStage, module: HuxerUIApplicationModule, quit: () => Promise<void>) {
    this.stage = stage;
    this.module = module;
    this.quit = quit;
    this.windows.set(this.mainId, stage.getMainWindowSync());
    this.module.setWindowStage(this.mainId, this);
  }

  id(context?: UIContext): string {
    return context?.getSharedLocalStorage()?.get<string>(windowStorageKey) ?? this.mainId;
  }

  get(id: string): window.Window | undefined {
    return this.windows.get(id);
  }

  async loadMain(): Promise<void> {
    const main = this.windows.get(this.mainId);
    if (!main) return;
    try {
      await main.setWindowLayoutFullScreen(true);
    } catch (error) {
      console.warn(`HuxerUI could not enable immersive layout: ${error}`);
    }
    if (this.windows.has(this.mainId)) {
      await this.stage.loadContent('pages/Index', this.storage(this.mainId));
    }
  }

  command(id: string, command: string, width: number, height: number): void {
    if (command === 'quit') {
      this.opening.clear();
      // Let the C++ shutdown callback return before retiring the ability.
      void Promise.resolve().then(this.quit).catch((error: Error): void => {
        console.warn(`HuxerUI could not terminate the OHOS ability: ${error}`);
      });
    } else if (command === 'open') {
      this.opening.add(id);
      void this.open(id, width, height);
    } else {
      void this.update(id, command).catch((error: Error): void => {
        console.warn(`HuxerUI could not ${command} an OHOS window: ${error}`);
      });
    }
  }

  forget(id: string): void {
    this.windows.delete(id);
    this.module.detach(id);
  }

  dispose(): void {
    this.module.setWindowStage(this.mainId, undefined);
    this.opening.clear();
    this.windows.delete(this.mainId);
    this.windows.forEach((_: window.Window, id: string): void => this.command(id, 'close', 0, 0));
  }

  failOpen(id: string, message: string): void {
    if (this.opening.delete(id)) {
      this.module.completeOpenWindow(id, message);
      this.command(id, 'close', 0, 0);
    }
  }

  private storage(id: string): LocalStorage {
    const storage = new LocalStorage();
    storage.setOrCreate(windowStorageKey, id);
    return storage;
  }

  private async open(id: string, width: number, height: number): Promise<void> {
    try {
      const child = await this.stage.createSubWindow('huxerui-' + id);
      // The creation promise may finish after the Task was canceled or its stage was destroyed.
      if (!this.opening.has(id)) {
        await child.destroyWindow();
        return;
      }
      this.windows.set(id, child);
      const bounds = this.stage.getMainWindowSync().getWindowProperties().windowRect;
      const scale = displayScale();
      const w = Math.max(1, Math.min(Math.round(width * scale), bounds.width));
      const h = Math.max(1, Math.min(Math.round(height * scale), bounds.height));
      try {
        await child.resize(w, h);
        await child.moveWindowTo(bounds.left + Math.floor((bounds.width - w) / 2),
          bounds.top + Math.floor((bounds.height - h) / 2));
      } catch (error) {
        console.warn(`HuxerUI could not set the preferred OHOS child window size or position: ${error}`);
      }
      if (!this.opening.has(id)) return;
      await child.loadContent('pages/Index', this.storage(id));
      if (!this.opening.has(id)) return;
      await child.showWindow();
      if (this.opening.delete(id)) {
        this.module.completeOpenWindow(id, undefined);
      }
    } catch (error) {
      this.failOpen(id, `HuxerUI could not open an OHOS window: ${error}`);
    }
  }

  private async update(id: string, command: string): Promise<void> {
    if (command === 'close') this.opening.delete(id);
    const child = this.windows.get(id);
    if (!child || this.closing.has(id)) return;
    switch (command) {
      case 'close':
        this.closing.add(id);
        try {
          await child.destroyWindow();
          this.forget(id);
        } finally {
          this.closing.delete(id);
        }
        break;
      case 'show':
        await child.showWindow();
        break;
      case 'activate':
        if (child.isWindowShowing()) {
          await child.raiseToAppTop();
        } else {
          await child.showWindow();
        }
        break;
      case 'hide':
        await child.minimize();
        break;
    }
  }
}

/** Hosts the HuxerUI application and loads `pages/Index` when its window is ready. */
export abstract class HuxerUIAbility extends UIAbility {
  private backgroundTasks?: HuxerUIBackgroundTasks;

  /** Returns the default export of the application's HuxerUI shared library, such as `libmy_app.so`. */
  protected abstract getApplicationModule(): HuxerUIApplicationModule;

  /** Supplies ArkTS factory objects for the C++ application's registered platform modules and views. */
  protected getPlatformExtensions(): HuxerUIPlatformExtensions | undefined { return undefined; }

  /** Maps C++ system task declaration identifiers to this ability's declared continuous background modes. */
  protected getSystemTaskModes(): Map<string, backgroundTaskManager.BackgroundMode> { return new Map(); }

  /** Returns the name of this app's declared HuxerUIBackgroundTaskExtensionAbility, when configured. */
  protected getBackgroundTaskAbility(): string | undefined { return undefined; }

  onCreate(want?: Want): void {
    const module = this.getApplicationModule();
    const ability = this.context.abilityInfo;
    const services = new HuxerUISystemServices(module, this.context, ability.bundleName, ability.name, this.context);
    const files = new HuxerUIFilePicker(module, this.context);
    const tasks = new HuxerUISystemTasks(module, this.context, this.getSystemTaskModes());
    const extension = this.getBackgroundTaskAbility();
    if (extension && canIUse('SystemCapability.ResourceSchedule.WorkScheduler')) {
      this.backgroundTasks = new HuxerUIBackgroundTasks(module, ability.bundleName, extension);
    }
    systemDark = this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    withActivation(want, activation => {
      module.start(this.context, i18n.System.getSystemLocale(), displayScale(), services, new HuxerUIHttp(module),
        activation, files, this.getPlatformExtensions(), tasks, this.backgroundTasks);
      this.backgroundTasks?.watch();
    });
    applicationModule = module;
  }

  onWindowStageCreate(stage: window.WindowStage): void {
    if (!applicationModule) return;
    const windows = new HuxerUIWindows(stage, applicationModule, () => {
      this.closeWindowStage();
      return this.context.terminateSelf();
    });
    applicationWindows = windows;
    void windows.loadMain().catch((error: Error): void => {
      console.error(`HuxerUI could not load the application page: ${error}`);
    });
  }

  onWindowStageDestroy(): void {
    this.closeWindowStage();
  }

  onForeground(): void {
    applicationModule?.setActive(true);
    this.backgroundTasks?.watch();
  }

  onBackground(): void {
    applicationModule?.setActive(false);
  }

  onConfigurationUpdate(configuration: Configuration = this.context.config): void {
    if (configuration.colorMode !== undefined) {
      systemDark = configuration.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    }
    applicationModule?.setConfiguration(i18n.System.getSystemLocale(), displayScale(), systemDark);
  }

  onNewWant(want: Want): void {
    withActivation(want, activation => {
      if (activation) {
        applicationModule?.activate(activation);
      }
    });
  }

  onDestroy(): void {
    this.closeWindowStage();
    this.backgroundTasks?.close();
    applicationModule?.stop();
    this.backgroundTasks = undefined;
    applicationModule = undefined;
  }

  private closeWindowStage(): void {
    const windows = applicationWindows;
    applicationWindows = undefined;
    windows?.dispose();
  }
}

/**
 * Forwards the page's back action to HuxerUI.
 * @param context The originating page's UI context. Omit it when forwarding only from the main window.
 * @returns True when HuxerUI handled the action.
 */
export function handleHuxerUIBack(context?: UIContext): boolean {
  const id = applicationWindows?.id(context);
  return id !== undefined && (applicationModule?.handleBack(id) ?? false);
}

/** Attaches one ArkUI page to its HuxerUI window in the running application. */
@Component
export struct HuxerUISurface {
  private surface: NodeContent = new NodeContent();
  private attachedWindow?: window.Window;
  private attachedId: string = '';

  private updatePresentation = (event?: window.WindowEventType): void => {
    const owner = this.attachedWindow;
    if (!owner) return;
    if (event === window.WindowEventType.WINDOW_DESTROYED) {
      applicationWindows?.forget(this.attachedId);
    } else {
      applicationModule?.setWindowPresentation(this.attachedId, owner.isWindowShowing(), owner.isFocused());
    }
  };

  private updateInsets = (): void => {
    const mainWindow = this.attachedWindow;
    if (!mainWindow) {
      return;
    }
    const properties = mainWindow.getWindowProperties();
    let top = 0;
    let right = 0;
    let bottom = 0;
    let left = 0;
    if (properties.isLayoutFullScreen) {
      for (const type of systemAvoidAreaTypes) {
        const area = mainWindow.getWindowAvoidArea(type);
        // A cutout rectangle can start inside the window edge.
        if (area.topRect.height > 0) {
          top = Math.max(top, area.topRect.top + area.topRect.height);
        }
        if (area.rightRect.width > 0) {
          right = Math.max(right, properties.windowRect.width - area.rightRect.left);
        }
        if (area.bottomRect.height > 0) {
          bottom = Math.max(bottom, properties.windowRect.height - area.bottomRect.top);
        }
        if (area.leftRect.width > 0) {
          left = Math.max(left, area.leftRect.left + area.leftRect.width);
        }
      }
    }
    applicationModule?.setSystemInsets(this.attachedId, top, right, bottom, left, properties.windowRect.height,
      this.getUIContext().vp2px(1));
  };

  build() {
    Column() {
      ContentSlot(this.surface)
    }
    .width('100%')
    .height('100%')
    // Use the ArkTS source tool to distinguish touchpad and mouse. Begin can repeat the first wheel delta.
    .onAxisEvent((event: AxisEvent): void => {
      if (this.attachedWindow && event.action === AxisAction.UPDATE) {
        const touchpad = event.sourceTool === SourceTool.TOUCHPAD || event.source === SourceType.TouchScreen;
        if (touchpad || event.source === SourceType.Mouse) {
          const consumed = applicationModule?.scroll(event.x, event.y, event.getHorizontalAxisValue(),
            event.getVerticalAxisValue(), touchpad, event.getModifierKeyState?.(['Shift']) ?? false,
            event.getModifierKeyState?.(['Ctrl']) ?? false, event.getModifierKeyState?.(['Alt']) ?? false,
            this.attachedId);
          if (consumed) {
            return;
          }
        }
      }
      event.propagation();
    })
    .onAttach(() => {
      const context = this.getUIContext();
      const windows = applicationWindows;
      const module = applicationModule;
      const id = windows?.id(context);
      const owner = id === undefined ? undefined : windows?.get(id);
      if (!windows || !module || !owner || id === undefined) return;
      const main = id === windows.mainId;
      const hostContext = context.getHostContext();
      if (!hostContext) return;
      const picker = new HuxerUIFilePicker(module, hostContext, true, id);
      context.setKeyboardAvoidMode(KeyboardAvoidMode.RESIZE);
      const textInput = new HuxerUITextInput(module, id, owner.getWindowProperties().id);
      try {
        module.attach(id, this.surface, context, context.vp2px(1), textInput, picker, systemDark, light => {
          if (main) void setStatusBarBrightness(owner, light);
        }, owner);
      } catch (error) {
        if (main) throw new Error(`HuxerUI could not attach the OHOS main window: ${error}`);
        windows.failOpen(id, `HuxerUI could not attach the OHOS child window: ${error}`);
        return;
      }
      this.attachedWindow = owner;
      this.attachedId = id;
      owner.on('avoidAreaChange', this.updateInsets);
      owner.on('windowSizeChange', this.updateInsets);
      owner.on('windowEvent', this.updatePresentation);
      this.updateInsets();
      this.updatePresentation();
    })
    .onDetach(() => {
      const owner = this.attachedWindow;
      if (!owner) return;
      const id = this.attachedId;
      this.attachedWindow = undefined;
      this.attachedId = '';
      try {
        owner.off('avoidAreaChange', this.updateInsets);
        owner.off('windowSizeChange', this.updateInsets);
        owner.off('windowEvent', this.updatePresentation);
      } catch (_) {
        // The system can destroy the window before ArkUI detaches the content.
      }
      applicationModule?.detach(id);
    })
  }
}
