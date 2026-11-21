import { UIAbility, Want, wantConstant } from '@kit.AbilityKit';
import { display, NodeContent, window } from '@kit.ArkUI';
import { i18n } from '@kit.LocalizationKit';
import { HuxerUIActivation, HuxerUIApplicationModule } from './HuxerUIApplicationModule';
import { HuxerUIFilePicker, withGrantedFiles } from './HuxerUIFilePicker';
import { HuxerUIHttp } from './HuxerUIHttp';
import { HuxerUISystemServices, notificationActivation } from './HuxerUISystemServices';
import { HuxerUITextInput } from './HuxerUITextInput';

let applicationModule: HuxerUIApplicationModule | undefined;
let filePicker: HuxerUIFilePicker | undefined;
let applicationWindowId: number | undefined;

function displayScale(): number {
  try {
    return display.getDefaultDisplaySync().densityPixels;
  } catch (_) {
    return 1;
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
  /** Returns the default export of the application's HuxerUI shared library, such as `libmy_app.so`. */
  protected abstract getApplicationModule(): HuxerUIApplicationModule;

  onCreate(want?: Want): void {
    const module = this.getApplicationModule();
    const services = new HuxerUISystemServices(module, this.context);
    const files = new HuxerUIFilePicker(module, this.context);
    withActivation(want, activation => {
      module.start(this.context, i18n.System.getSystemLocale(), displayScale(), services, new HuxerUIHttp(module),
        activation, files);
    });
    applicationModule = module;
    filePicker = files;
  }

  onWindowStageCreate(stage: window.WindowStage): void {
    applicationWindowId = stage.getMainWindowSync().getWindowProperties().id;
    stage.loadContent('pages/Index');
  }

  onForeground(): void {
    applicationModule?.setActive(true);
  }

  onBackground(): void {
    applicationModule?.setActive(false);
  }

  onConfigurationUpdate(): void {
    applicationModule?.setConfiguration(i18n.System.getSystemLocale(), displayScale());
  }

  onNewWant(want: Want): void {
    withActivation(want, activation => {
      if (activation) {
        applicationModule?.activate(activation);
      }
    });
  }

  onDestroy(): void {
    applicationModule?.stop();
    applicationModule = undefined;
    filePicker = undefined;
    applicationWindowId = undefined;
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

  build() {
    Column() {
      ContentSlot(this.surface)
    }
    .width('100%')
    .height('100%')
    // ArkUI's ArkTS callback preserves the source tool on API 19. Begin can repeat the first wheel delta.
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
      if (!applicationModule || !filePicker) {
        throw new Error('HuxerUI surface requires a running application');
      }
      const context = this.getUIContext();
      const textInput = new HuxerUITextInput(applicationModule, applicationWindowId);
      applicationModule.attach(this.surface, context, context.vp2px(1), textInput, filePicker);
    })
    .onDetach(() => applicationModule?.detach())
  }
}
