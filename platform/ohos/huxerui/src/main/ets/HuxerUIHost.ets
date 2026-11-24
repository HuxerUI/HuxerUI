import { Configuration, ConfigurationConstant, UIAbility, Want, wantConstant } from '@kit.AbilityKit';
import { display, KeyboardAvoidMode, NodeContent, window } from '@kit.ArkUI';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { i18n } from '@kit.LocalizationKit';
import { HuxerUIActivation, HuxerUIApplicationModule } from './HuxerUIApplicationModule';
import { HuxerUIBackgroundTasks } from './HuxerUIBackgroundTasks';
import { HuxerUIFilePicker, withGrantedFiles } from './HuxerUIFilePicker';
import { HuxerUIHttp } from './HuxerUIHttp';
import { HuxerUIPlatformExtensions } from './HuxerUIPlatform';
import { HuxerUISystemServices, notificationActivation } from './HuxerUISystemServices';
import { HuxerUISystemTasks } from './HuxerUISystemTasks';
import { HuxerUITextInput } from './HuxerUITextInput';

let applicationModule: HuxerUIApplicationModule | undefined;
let filePicker: HuxerUIFilePicker | undefined;
let applicationWindow: window.Window | undefined;
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
    filePicker = files;
  }

  onWindowStageCreate(stage: window.WindowStage): void {
    const mainWindow = stage.getMainWindowSync();
    applicationWindow = mainWindow;
    void this.loadWindowContent(stage, mainWindow);
  }

  onWindowStageDestroy(): void {
    applicationWindow = undefined;
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
    this.backgroundTasks?.close();
    applicationModule?.stop();
    this.backgroundTasks = undefined;
    applicationModule = undefined;
    filePicker = undefined;
    applicationWindow = undefined;
  }

  private async loadWindowContent(stage: window.WindowStage, mainWindow: window.Window): Promise<void> {
    try {
      await mainWindow.setWindowLayoutFullScreen(true);
    } catch (error) {
      console.warn(`HuxerUI could not enable immersive layout: ${error}`);
    }
    if (applicationWindow === mainWindow) {
      try {
        await stage.loadContent('pages/Index');
      } catch (error) {
        console.error(`HuxerUI could not load the application page: ${error}`);
      }
    }
  }
}

/**
 * Forwards the page's back action to HuxerUI.
 * @returns True when HuxerUI handled the action.
 */
export function handleHuxerUIBack(): boolean {
  return applicationModule?.handleBack() ?? false;
}

/** Attaches the running application to the ArkUI page. Each Runtime supports one attached surface at a time. */
@Component
export struct HuxerUISurface {
  private surface: NodeContent = new NodeContent();
  private attachedWindow?: window.Window;

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
    applicationModule?.setSystemInsets(top, right, bottom, left, properties.windowRect.height);
  };

  build() {
    Column() {
      ContentSlot(this.surface)
    }
    .width('100%')
    .height('100%')
    // Use the ArkTS source tool to distinguish touchpad and mouse. Begin can repeat the first wheel delta.
    .onAxisEvent((event: AxisEvent): void => {
      if (event.action === AxisAction.UPDATE) {
        const touchpad = event.sourceTool === SourceTool.TOUCHPAD || event.source === SourceType.TouchScreen;
        if (touchpad || event.source === SourceType.Mouse) {
          const consumed = applicationModule?.scroll(event.x, event.y, event.getHorizontalAxisValue(),
            event.getVerticalAxisValue(), touchpad, event.getModifierKeyState?.(['Shift']) ?? false,
            event.getModifierKeyState?.(['Ctrl']) ?? false, event.getModifierKeyState?.(['Alt']) ?? false);
          if (consumed) {
            return;
          }
        }
      }
      event.propagation();
    })
    .onAttach(() => {
      const mainWindow = applicationWindow;
      if (!applicationModule || !filePicker || !mainWindow) {
        throw new Error('HuxerUI surface requires a running application');
      }
      const context = this.getUIContext();
      context.setKeyboardAvoidMode(KeyboardAvoidMode.RESIZE);
      const textInput = new HuxerUITextInput(applicationModule, mainWindow.getWindowProperties().id);
      applicationModule.attach(this.surface, context, context.vp2px(1), textInput, filePicker, systemDark,
        light => setStatusBarBrightness(mainWindow, light), mainWindow);
      this.attachedWindow = mainWindow;
      mainWindow.on('avoidAreaChange', this.updateInsets);
      mainWindow.on('windowSizeChange', this.updateInsets);
      this.updateInsets();
    })
    .onDetach(() => {
      const mainWindow = this.attachedWindow;
      this.attachedWindow = undefined;
      mainWindow?.off('avoidAreaChange', this.updateInsets);
      mainWindow?.off('windowSizeChange', this.updateInsets);
      applicationModule?.detach();
    })
  }
}
