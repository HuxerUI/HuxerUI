import { WorkSchedulerExtensionAbility, workScheduler } from '@kit.BackgroundTasksKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { i18n } from '@kit.LocalizationKit';
import { HuxerUIApplicationModule, HuxerUIBackgroundTaskHost } from './HuxerUIApplicationModule';
import { HuxerUIFilePicker } from './HuxerUIFilePicker';
import { HuxerUIHttp } from './HuxerUIHttp';
import { HuxerUISystemServices } from './HuxerUISystemServices';

// Match the application library's WorkScheduler result codes.
enum WorkResult { Accepted, Unsupported, Configuration, Rejected, Platform }

export class HuxerUIBackgroundTasks implements HuxerUIBackgroundTaskHost {
  private module: HuxerUIApplicationModule;
  private bundleName: string;
  private abilityName: string;
  private interval: number;
  private timer?: number;
  private nextReconciliation = 0;
  private closed = false;

  constructor(module: HuxerUIApplicationModule, bundleName: string, abilityName: string, worker: boolean = false) {
    this.module = module;
    this.bundleName = bundleName;
    this.abilityName = abilityName;
    this.interval = worker ? 200 : 500;
  }

  submit(id: number, delay: number, network: boolean, power: boolean): WorkResult {
    if ((!network && !power) || (delay > 0 && deviceInfo.sdkApiVersion < 22)) return WorkResult.Unsupported;
    const work = this.workInfo(id, network, power);
    work.isPersisted = true;
    if (delay > 0) work.earliestStartTime = delay;
    try {
      workScheduler.startWork(work);
      this.watch();
      return WorkResult.Accepted;
    } catch (error) {
      const failure = error as BusinessError;
      console.error(`HuxerUI could not schedule OHOS background work ${id}: ${failure.code}: ${failure.message}`);
      if (failure.code === 801) return WorkResult.Unsupported;
      if (failure.code === 401 || failure.code === 9700004) return WorkResult.Configuration;
      return failure.code === 9700005 ? WorkResult.Rejected : WorkResult.Platform;
    }
  }

  withdraw(id: number, network: boolean, power: boolean): WorkResult {
    try {
      workScheduler.stopWork(this.workInfo(id, network, power), true);
      return WorkResult.Accepted;
    } catch (error) {
      console.warn(`HuxerUI could not remove OHOS background work ${id}: ${error}`);
      return WorkResult.Platform;
    }
  }

  watch(): void {
    if (this.closed || this.timer !== undefined) return;
    this.timer = setTimeout(() => {
      this.timer = undefined;
      try {
        const pending = this.module.readBackgroundWork();
        if (pending === undefined) return;
        if (pending.length > 0 && Date.now() >= this.nextReconciliation) {
          this.nextReconciliation = Date.now() + 15000;
          void this.reconcile(pending);
        }
      } catch (error) {
        console.error(`HuxerUI could not observe OHOS background work: ${error}`);
      }
      this.watch();
    }, this.interval);
  }

  close(): void {
    this.closed = true;
    if (this.timer !== undefined) clearTimeout(this.timer);
    this.timer = undefined;
  }

  private workInfo(id: number, network: boolean, power: boolean): workScheduler.WorkInfo {
    const work: workScheduler.WorkInfo = { workId: id, bundleName: this.bundleName, abilityName: this.abilityName };
    if (network) work.networkType = workScheduler.NetworkType.NETWORK_TYPE_ANY;
    if (power) {
      work.isCharging = true;
      work.chargerType = workScheduler.ChargingType.CHARGING_PLUGGED_ANY;
    }
    return work;
  }

  private async reconcile(pending: number[]): Promise<void> {
    try {
      const known = await workScheduler.obtainAllWorks();
      if (this.closed) return;
      const missing = pending.filter(id =>
        !known.some(work => work.workId === id && work.abilityName === this.abilityName));
      if (missing.length > 0) this.module.reconcileBackgroundWork(missing);
    } catch (error) {
      console.warn(`HuxerUI could not reconcile OHOS background work: ${error}`);
    }
  }
}

/** Executes configured C++ BackgroundTask handlers in the WorkScheduler process without creating a window. */
export abstract class HuxerUIBackgroundTaskExtensionAbility extends WorkSchedulerExtensionAbility {
  private module?: HuxerUIApplicationModule;
  private tasks?: HuxerUIBackgroundTasks;

  /** Returns the same application library used by the entry HuxerUIAbility. */
  protected abstract getApplicationModule(): HuxerUIApplicationModule;

  onWorkStart(work: workScheduler.WorkInfo): void {
    try {
      const module = this.getApplicationModule();
      if (!module.prepareBackgroundWork(this.context.filesDir, work.workId)) return;
      if (!this.module) {
        const context = this.context;
        const tasks = new HuxerUIBackgroundTasks(module, work.bundleName, work.abilityName, true);
        const entryAbility = context.currentHapModuleInfo?.mainElementName ?? '';
        const services = new HuxerUISystemServices(module, context, work.bundleName, entryAbility);
        module.start(context, i18n.System.getSystemLocale(), 1, services, new HuxerUIHttp(module), undefined,
          new HuxerUIFilePicker(module, context, false), undefined, undefined, tasks);
        this.module = module;
        this.tasks = tasks;
      }
      module.backgroundWorkEvent(work.workId, true);
      this.tasks?.watch();
    } catch (error) {
      console.error(`HuxerUI could not start OHOS background work ${work.workId}: ${error}`);
    }
  }

  onWorkStop(work: workScheduler.WorkInfo): void {
    try {
      this.module?.backgroundWorkEvent(work.workId, false);
    } catch (error) {
      console.error(`HuxerUI could not stop OHOS background work ${work.workId}: ${error}`);
    }
  }

  onDestroy(): void {
    this.tasks?.close();
    this.module?.stop();
    this.tasks = undefined;
    this.module = undefined;
  }
}
