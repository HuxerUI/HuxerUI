import { bundleManager, common, WantAgent, wantAgent } from '@kit.AbilityKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { notificationManager } from '@kit.NotificationKit';
import { HuxerUIApplicationModule, HuxerUISystemTaskHost } from './HuxerUIApplicationModule';

// Match the application library's continuous task bridge codes.
enum TaskEvent { Granted, Rejected, Revoked }
enum TaskResult { Accepted, Unavailable, NotPermitted, Configuration, Busy, Rejected, Platform }

function modeName(mode: backgroundTaskManager.BackgroundMode): string | undefined {
  switch (mode) {
    case backgroundTaskManager.BackgroundMode.DATA_TRANSFER: return 'dataTransfer';
    case backgroundTaskManager.BackgroundMode.AUDIO_PLAYBACK: return 'audioPlayback';
    case backgroundTaskManager.BackgroundMode.AUDIO_RECORDING: return 'audioRecording';
    case backgroundTaskManager.BackgroundMode.LOCATION: return 'location';
    case backgroundTaskManager.BackgroundMode.BLUETOOTH_INTERACTION: return 'bluetoothInteraction';
    case backgroundTaskManager.BackgroundMode.MULTI_DEVICE_CONNECTION: return 'multiDeviceConnection';
    default: return undefined;
  }
}

function configuredForContinuousTasks(): boolean {
  try {
    if (!canIUse('SystemCapability.ResourceSchedule.BackgroundTaskManager.ContinuousTask')) return false;
    const flags = bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_REQUESTED_PERMISSION;
    const bundle = bundleManager.getBundleInfoForSelfSync(flags);
    return bundle.reqPermissionDetails.some(item => item.name === 'ohos.permission.KEEP_BACKGROUND_RUNNING');
  } catch (_) {
    return false;
  }
}

function taskFailure(error: BusinessError): TaskResult {
  switch (error.code) {
    case 201: return TaskResult.NotPermitted;
    case 401: return TaskResult.Configuration;
    case 801: return TaskResult.Unavailable;
    case 9800005:
    case 9800006: return TaskResult.Rejected;
    default: return TaskResult.Platform;
  }
}

class ContinuousTask {
  token: string;
  mode: backgroundTaskManager.BackgroundMode;
  title: string;
  subtitle: string;
  id?: number;
  notificationId?: number;
  releasing = false;
  stopping = false;
  publishing = false;
  dirty = false;
  percent?: number;

  constructor(token: string, mode: backgroundTaskManager.BackgroundMode, title: string, subtitle: string) {
    this.token = token;
    this.mode = mode;
    this.title = title;
    this.subtitle = subtitle;
  }
}

export class HuxerUISystemTasks implements HuxerUISystemTaskHost {
  readonly canContinue: boolean;
  readonly canRunOngoing: boolean;
  private module: HuxerUIApplicationModule;
  private context: common.UIAbilityContext;
  private modes: Map<string, backgroundTaskManager.BackgroundMode>;
  private multiple = deviceInfo.sdkApiVersion >= 21;
  private subscribed = false;
  private closed = false;
  private activation?: WantAgent;
  private tasks = new Map<string, ContinuousTask>();
  // Cancellation or suspension can arrive before the promise exposes the corresponding system ID.
  private earlyStops = new Map<number, boolean>();
  private onCancel = (info: backgroundTaskManager.ContinuousTaskCancelInfo): void => this.systemStop(info.id, false);
  private onSuspend = (info: backgroundTaskManager.ContinuousTaskSuspendInfo): void =>
    this.systemStop(info.continuousTaskId, true);

  constructor(module: HuxerUIApplicationModule, context: common.UIAbilityContext,
    modes: Map<string, backgroundTaskManager.BackgroundMode>) {
    this.module = module;
    this.context = context;
    this.modes = new Map(modes);
    const configured = modes.size > 0 && configuredForContinuousTasks();
    const supported = configured ? Array.from(modes.values()).filter(mode => modeName(mode) !== undefined) : [];
    this.canContinue = configured && supported.includes(backgroundTaskManager.BackgroundMode.DATA_TRANSFER);
    this.canRunOngoing = configured &&
      supported.some(mode => mode !== backgroundTaskManager.BackgroundMode.DATA_TRANSFER);
    if (this.canContinue || this.canRunOngoing) void this.prepare();
  }

  submit(token: string, identifier: string, ongoing: boolean, title: string, subtitle: string): TaskResult {
    const mode = this.modes.get(identifier);
    if (mode === undefined || modeName(mode) === undefined ||
      ongoing === (mode === backgroundTaskManager.BackgroundMode.DATA_TRANSFER)) return TaskResult.Configuration;
    if (this.closed || !this.subscribed || !this.activation) return TaskResult.Unavailable;
    for (const task of this.tasks.values()) {
      if (task.releasing) this.stop(task);
    }
    if (this.tasks.size >= (this.multiple ? 10 : 1)) return TaskResult.Busy;
    const task = new ContinuousTask(token, mode, title, subtitle);
    this.tasks.set(token, task);
    void this.start(task, this.activation);
    return TaskResult.Accepted;
  }

  release(token: string): TaskResult {
    const task = this.tasks.get(token);
    if (task) {
      task.releasing = true;
      this.stop(task);
    }
    return TaskResult.Accepted;
  }

  reportProgress(token: string, percent: number): TaskResult {
    const task = this.tasks.get(token);
    if (!task || task.releasing) return TaskResult.Unavailable;
    if (task.mode === backgroundTaskManager.BackgroundMode.DATA_TRANSFER && percent >= 0) {
      task.percent = Math.min(100, Math.floor(percent));
      this.queueProgress(task);
    }
    return TaskResult.Accepted;
  }

  updatePresentation(token: string, title: string, subtitle: string): TaskResult {
    const task = this.tasks.get(token);
    if (!task || task.releasing) return TaskResult.Unavailable;
    task.title = title;
    task.subtitle = subtitle;
    if (task.percent !== undefined) this.queueProgress(task);
    return TaskResult.Accepted;
  }

  close(): TaskResult {
    this.closed = true;
    for (const task of this.tasks.values()) this.release(task.token);
    if (this.subscribed) {
      this.subscribed = false;
      backgroundTaskManager.off('continuousTaskCancel', this.onCancel);
      backgroundTaskManager.off('continuousTaskSuspend', this.onSuspend);
    }
    return TaskResult.Accepted;
  }

  private async prepare(): Promise<void> {
    try {
      const ability = this.context.abilityInfo;
      this.activation = await wantAgent.getWantAgent({
        wants: [{ bundleName: ability.bundleName, moduleName: ability.moduleName, abilityName: ability.name }],
        actionType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      });
      if (this.closed) return;
      backgroundTaskManager.on('continuousTaskCancel', this.onCancel);
      try {
        backgroundTaskManager.on('continuousTaskSuspend', this.onSuspend);
      } catch (error) {
        backgroundTaskManager.off('continuousTaskCancel', this.onCancel);
        throw new Error(`could not subscribe to continuous task suspension: ${error}`);
      }
      this.subscribed = true;
    } catch (error) {
      console.error(`HuxerUI could not configure OHOS continuous tasks: ${error}`);
    }
  }

  private async start(task: ContinuousTask, activation: WantAgent): Promise<void> {
    let grant: backgroundTaskManager.ContinuousTaskNotification;
    try {
      if (this.multiple) {
        const request = new backgroundTaskManager.ContinuousTaskRequest();
        // The supported legacy modes have the same values in the API 21 mode enum.
        request.backgroundTaskModes = [task.mode as number];
        request.backgroundTaskSubmodes = [task.mode === backgroundTaskManager.BackgroundMode.DATA_TRANSFER ?
          backgroundTaskManager.BackgroundTaskSubmode.SUBMODE_LIVE_VIEW_NOTIFICATION :
          backgroundTaskManager.BackgroundTaskSubmode.SUBMODE_NORMAL_NOTIFICATION];
        request.wantAgent = activation;
        grant = await backgroundTaskManager.startBackgroundRunning(this.context, request);
      } else {
        const mode = modeName(task.mode) as string;
        grant = await backgroundTaskManager.startBackgroundRunning(this.context, [mode], activation);
      }
    } catch (error) {
      const failure = error as BusinessError;
      this.tasks.delete(task.token);
      this.clearEarlyStops();
      if (!task.releasing && !this.closed) {
        this.module.systemTaskEvent(task.token, TaskEvent.Rejected, taskFailure(failure),
          `${failure.code}: ${failure.message}`);
      }
      return;
    }
    const id = grant.continuousTaskId ?? -1;
    if (this.multiple && id < 0) {
      this.tasks.delete(task.token);
      this.clearEarlyStops();
      if (!task.releasing && !this.closed) {
        this.module.systemTaskEvent(task.token, TaskEvent.Rejected, TaskResult.Platform, 'Missing system task ID');
      }
      return;
    }
    task.id = id;
    task.notificationId = grant.notificationId;
    const earlyStop = this.earlyStops.get(id);
    this.earlyStops.delete(id);
    this.clearEarlyStops();
    if (earlyStop !== undefined) {
      this.systemStop(id, earlyStop);
    } else if (task.releasing) {
      this.stop(task);
    } else {
      this.module.systemTaskEvent(task.token, TaskEvent.Granted, TaskResult.Accepted);
    }
  }

  private clearEarlyStops(): void {
    if (!Array.from(this.tasks.values()).some(task => task.id === undefined)) this.earlyStops.clear();
  }

  private systemStop(id: number, suspended: boolean): void {
    for (const task of this.tasks.values()) {
      if (task.id !== id) continue;
      const notify = !task.releasing && !this.closed;
      task.releasing = true;
      if (suspended) {
        this.stop(task);
      } else if (!task.stopping) {
        this.tasks.delete(task.token);
      }
      if (notify) this.module.systemTaskEvent(task.token, TaskEvent.Revoked, TaskResult.Rejected);
      return;
    }
    if (Array.from(this.tasks.values()).some(task => task.id === undefined)) this.earlyStops.set(id, suspended);
  }

  private stop(task: ContinuousTask): void {
    if (task.id === undefined || task.stopping || task.publishing) return;
    task.stopping = true;
    void this.finishStop(task, task.id);
  }

  private async finishStop(task: ContinuousTask, id: number): Promise<void> {
    try {
      if (this.multiple) await backgroundTaskManager.stopBackgroundRunning(this.context, id);
      else await backgroundTaskManager.stopBackgroundRunning(this.context);
      this.tasks.delete(task.token);
    } catch (error) {
      // Keep the slot until the system confirms this grant is gone. An API 20 stop targets the entire ability.
      try {
        const active = this.multiple ? await backgroundTaskManager.getAllContinuousTasks(this.context, true) :
          await backgroundTaskManager.getAllContinuousTasks(this.context);
        if (!active.some(entry => entry.continuousTaskId === id)) {
          this.tasks.delete(task.token);
          return;
        }
      } catch (_) {
      }
      console.error(`HuxerUI could not release OHOS continuous task ${id}: ${error}`);
    } finally {
      task.stopping = false;
    }
  }

  private queueProgress(task: ContinuousTask): void {
    task.dirty = true;
    if (!task.publishing && task.notificationId !== undefined && task.notificationId >= 0) {
      task.publishing = true;
      void this.publishProgress(task, task.notificationId);
    }
  }

  private async publishProgress(task: ContinuousTask, id: number): Promise<void> {
    // Only the latest progress needs publication while the preceding system update is in flight.
    while (task.dirty && !task.releasing) {
      task.dirty = false;
      try {
        await notificationManager.publish({
          id: id,
          notificationSlotType: notificationManager.SlotType.LIVE_VIEW,
          content: {
            notificationContentType: notificationManager.ContentType.NOTIFICATION_CONTENT_SYSTEM_LIVE_VIEW,
            systemLiveView: { typeCode: 8, title: task.title, text: task.subtitle },
          },
          template: {
            name: 'downloadTemplate',
            data: { title: task.title, fileName: task.subtitle || task.title, progressValue: task.percent },
          },
        });
      } catch (error) {
        console.error(`HuxerUI could not update OHOS continuous task progress: ${error}`);
      }
    }
    task.publishing = false;
    if (task.releasing && this.tasks.has(task.token)) this.stop(task);
  }
}
