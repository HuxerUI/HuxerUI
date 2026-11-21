import { UIAbility } from '@kit.AbilityKit';
import { NodeContent, window } from '@kit.ArkUI';
import { HuxerUIApplicationModule } from './HuxerUIApplicationModule';

let applicationModule: HuxerUIApplicationModule | undefined;

export abstract class HuxerUIAbility extends UIAbility {
  protected abstract getApplicationModule(): HuxerUIApplicationModule;

  onCreate(): void {
    const module = this.getApplicationModule();
    module.start();
    applicationModule = module;
  }

  onWindowStageCreate(stage: window.WindowStage): void {
    stage.loadContent('pages/Index');
  }

  onForeground(): void {
    applicationModule?.setActive(true);
  }

  onBackground(): void {
    applicationModule?.setActive(false);
  }

  onDestroy(): void {
    applicationModule?.stop();
    applicationModule = undefined;
  }
}

export function handleHuxerUIBack(): boolean {
  return applicationModule?.handleBack() ?? false;
}

@Component
export struct HuxerUISurface {
  private surface: NodeContent = new NodeContent();

  build() {
    Column() {
      ContentSlot(this.surface)
    }
    .width('100%')
    .height('100%')
    .onAttach(() => {
      if (!applicationModule) {
        throw new Error('HuxerUI surface requires a running application');
      }
      const context = this.getUIContext();
      applicationModule.attach(this.surface, context, context.vp2px(1));
    })
    .onDetach(() => applicationModule?.detach())
  }
}
