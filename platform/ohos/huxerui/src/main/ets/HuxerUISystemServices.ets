import { abilityAccessCtrl, bundleManager, common, Permissions, Want, wantAgent } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { reminderAgentManager } from '@kit.BackgroundTasksKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { notificationManager } from '@kit.NotificationKit';
import { HuxerUIApplicationModule, HuxerUINotificationActivation, HuxerUISystemServicesHost } from './HuxerUIApplicationModule';

// Match the application library's operation and result codes.
enum Operation { Check, Request, Settings, Show, Cancel, Schedule }
enum PermissionStatus { Granted = 1, Denied = 2, PermanentlyDenied = 3, Unavailable = 5, Limited = 7 }
enum NotificationStatus { Accepted, Unauthorized, Unavailable, Failed }

const approximateLocation: Permissions = 'ohos.permission.APPROXIMATELY_LOCATION';
const preciseLocation: Permissions = 'ohos.permission.LOCATION';
const notificationLabelPrefix = 'huxerui:';
const notificationAction = 'org.huxerui.action.NOTIFICATION';
const reminderPermission: Permissions = 'ohos.permission.PUBLISH_AGENT_REMINDER';
const firstReminderNotificationId = 0x40000000;

function notificationUri(identifier: string): string {
  return 'huxerui://notification/' + encodeURIComponent(identifier);
}

function reminderFailure(error: BusinessError): NotificationStatus {
  if (error.code === 1700001) {
    return NotificationStatus.Unauthorized;
  }
  return error.code === 201 || error.code === 801 ? NotificationStatus.Unavailable : NotificationStatus.Failed;
}

export function notificationActivation(want?: Want): HuxerUINotificationActivation | undefined {
  const identifier = want?.parameters?.['huxerui.notification.identifier'];
  const payload = want?.parameters?.['huxerui.notification.payload'];
  if (typeof identifier === 'string' && identifier.length > 0 &&
    typeof payload === 'string' && payload.length > 0 && payload.length <= 128 * 1024 &&
    (want?.action === notificationAction || want?.uri === notificationUri(identifier))) {
    return { identifier: identifier, payload: payload };
  }
  return undefined;
}

export class HuxerUISystemServices implements HuxerUISystemServicesHost {
  readonly canSchedule: boolean;
  private module: HuxerUIApplicationModule;
  private context: common.UIAbilityContext;

  constructor(module: HuxerUIApplicationModule, context: common.UIAbilityContext) {
    this.module = module;
    this.context = context;
    try {
      this.canSchedule = canIUse('SystemCapability.Notification.ReminderAgent') &&
        this.declaredPermissions(reminderPermission, this.getBundleInfo()).length > 0;
    } catch (_) {
      this.canSchedule = false;
    }
  }

  async perform(token: string, operation: Operation, args: string[]): Promise<void> {
    let result: number;
    try {
      switch (operation) {
        case Operation.Check:
        case Operation.Request:
          result = await this.permissionStatus(args[0], operation === Operation.Request);
          break;
        case Operation.Settings:
          result = await this.openPermissionSettings(args[0], args[1] === 'application') ? 1 : 0;
          break;
        case Operation.Show:
          result = await this.showNotification(args[0], args[1], args[2], args[3]);
          break;
        case Operation.Cancel:
          result = await this.cancelNotification(args[0]);
          break;
        case Operation.Schedule:
          result = await this.scheduleNotification(args[0], args[1], args[2], args[3], Number(args[4]));
          break;
        default:
          result = -1;
          break;
      }
    } catch (_) {
      result = -1;
    }
    this.module.completeSystemService(token, result);
  }

  private getBundleInfo(): bundleManager.BundleInfo {
    return bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION |
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_REQUESTED_PERMISSION);
  }

  private declaredPermissions(names: string, info: bundleManager.BundleInfo): Permissions[] {
    const permissions = names.split(',') as Permissions[];
    return permissions.filter((name: Permissions): boolean => info.reqPermissionDetails.some(
      (item: bundleManager.ReqPermissionDetail): boolean => item.name === name));
  }

  private async permissionStatus(names: string, request: boolean): Promise<PermissionStatus> {
    if (names === 'notifications') {
      return this.notificationPermission(request);
    }
    const info = this.getBundleInfo();
    const permissions = this.declaredPermissions(names, info);
    const requirePrecise = names.includes(preciseLocation);
    if (permissions.length === 0 || (!requirePrecise && permissions.length !== names.split(',').length)) {
      return PermissionStatus.Unavailable;
    }
    const manager = abilityAccessCtrl.createAtManager();
    const token = info.appInfo.accessTokenId;
    let permanentlyDenied = false;
    if (request && permissions.some((name: Permissions): boolean =>
      manager.checkAccessTokenSync(token, name) !== abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED)) {
      const response = await manager.requestPermissionsFromUser(this.context, permissions);
      permanentlyDenied = response.authResults.some((result: number, index: number): boolean =>
        result === abilityAccessCtrl.GrantStatus.PERMISSION_DENIED && response.dialogShownResults?.[index] === false);
    }
    if (permissions.length === names.split(',').length && permissions.every((name: Permissions): boolean =>
      manager.checkAccessTokenSync(token, name) === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED)) {
      return PermissionStatus.Granted;
    }
    if (requirePrecise && permissions.includes(approximateLocation) &&
      manager.checkAccessTokenSync(token, approximateLocation) === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED) {
      return PermissionStatus.Limited;
    }
    return permanentlyDenied ? PermissionStatus.PermanentlyDenied : PermissionStatus.Denied;
  }

  private async openPermissionSettings(names: string, application: boolean): Promise<boolean> {
    if (!application) {
      return false;
    }
    if (names === 'notifications') {
      return await this.notificationPermission(true) === PermissionStatus.Granted;
    }
    const permissions = this.declaredPermissions(names, this.getBundleInfo());
    if (permissions.length === 0) {
      return false;
    }
    await abilityAccessCtrl.createAtManager().requestPermissionOnSetting(this.context, permissions);
    return true;
  }

  private async notificationPermission(request: boolean): Promise<PermissionStatus> {
    if (await notificationManager.isNotificationEnabled()) {
      return PermissionStatus.Granted;
    }
    if (request) {
      try {
        await notificationManager.requestEnableNotification(this.context);
      } catch (_) {}
      if (await notificationManager.isNotificationEnabled()) {
        return PermissionStatus.Granted;
      }
    }
    return PermissionStatus.Denied;
  }

  private async showNotification(identifier: string, title: string, body: string,
    payload: string): Promise<NotificationStatus> {
    if (!await notificationManager.isNotificationEnabled()) {
      return NotificationStatus.Unauthorized;
    }
    try {
      const destination: Want = {
        bundleName: this.context.abilityInfo.bundleName,
        abilityName: this.context.abilityInfo.name,
        action: notificationAction,
        uri: notificationUri(identifier),
        parameters: {
          'huxerui.notification.identifier': identifier,
          'huxerui.notification.payload': payload
        }
      };
      const activation = await wantAgent.getWantAgent({
        wants: [destination],
        actionType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      });
      await notificationManager.publish({
        // The label pairs with the ID for both replacement and cancellation across process restarts.
        id: 0,
        label: notificationLabelPrefix + identifier,
        wantAgent: activation,
        content: {
          notificationContentType: notificationManager.ContentType.NOTIFICATION_CONTENT_BASIC_TEXT,
          normal: { title: title, text: body }
        }
      });
      await this.removeReminder(identifier);
      return NotificationStatus.Accepted;
    } catch (error) {
      const code = (error as BusinessError).code;
      return code === 1600004 ? NotificationStatus.Unauthorized : NotificationStatus.Failed;
    }
  }

  private async scheduleNotification(identifier: string, title: string, body: string,
    payload: string, deadline: number): Promise<NotificationStatus> {
    if (!this.canSchedule) {
      return NotificationStatus.Unavailable;
    }
    if (!await notificationManager.isNotificationEnabled()) {
      return NotificationStatus.Unauthorized;
    }
    try {
      const store = await preferences.getPreferences(this.context, { name: 'huxerui_notifications' });
      const key = notificationLabelPrefix + identifier;
      let id = await store.get(key, 0) as number;
      if (id === 0) {
        id = await store.get('next-id', firstReminderNotificationId) as number;
        if (id >= 0x7fffffff) {
          return NotificationStatus.Failed;
        }
        // Reserve the ID before publishing so cancellation can still find a delivered notification after restart.
        await store.put('next-id', id + 1);
        await store.put(key, id);
        await store.flush();
      }
      const pending = await this.pendingReminder(id);
      const seconds = Math.max(1, Math.ceil((deadline - Date.now()) / 1000));
      if (seconds > 0x7fffffff) {
        return NotificationStatus.Failed;
      }
      const request: reminderAgentManager.ReminderRequestTimer = {
        reminderType: reminderAgentManager.ReminderType.REMINDER_TYPE_TIMER,
        triggerTimeInSeconds: seconds,
        notificationId: id,
        title: title,
        content: body,
        expiredContent: body,
        ringDuration: 0,
        snoozeTimes: 0,
        slotType: notificationManager.SlotType.SERVICE_INFORMATION,
        wantAgent: {
          pkgName: this.context.abilityInfo.bundleName,
          abilityName: this.context.abilityInfo.name,
          uri: notificationUri(identifier),
          parameters: {
            'huxerui.notification.identifier': identifier,
            'huxerui.notification.payload': payload
          }
        }
      };
      if (pending !== undefined) {
        try {
          await reminderAgentManager.updateReminder(pending, request);
          return NotificationStatus.Accepted;
        } catch (error) {
          if ((error as BusinessError).code !== 1700003) {
            return reminderFailure(error as BusinessError);
          }
        }
      }
      await reminderAgentManager.publishReminder(request);
      return NotificationStatus.Accepted;
    } catch (error) {
      return reminderFailure(error as BusinessError);
    }
  }

  private async cancelNotification(identifier: string): Promise<NotificationStatus> {
    try {
      await this.removeReminder(identifier);
      await this.removeDeliveredNotification(0, notificationLabelPrefix + identifier);
      return NotificationStatus.Accepted;
    } catch (_) {
      return NotificationStatus.Failed;
    }
  }

  private async pendingReminder(id: number): Promise<number | undefined> {
    const reminders = await reminderAgentManager.getAllValidReminders();
    return reminders.find((item: reminderAgentManager.ReminderInfo): boolean =>
      item.reminderReq.notificationId === id)?.reminderId;
  }

  private async cancelPendingReminder(id: number): Promise<void> {
    try {
      await reminderAgentManager.cancelReminder(id);
    } catch (error) {
      if ((error as BusinessError).code !== 1700003) {
        throw new Error('HuxerUI could not cancel the OHOS reminder');
      }
    }
  }

  private async removeReminder(identifier: string): Promise<void> {
    if (!this.canSchedule) {
      return;
    }
    const store = await preferences.getPreferences(this.context, { name: 'huxerui_notifications' });
    const key = notificationLabelPrefix + identifier;
    const id = await store.get(key, 0) as number;
    if (id === 0) {
      return;
    }
    const pending = await this.pendingReminder(id);
    if (pending !== undefined) {
      await this.cancelPendingReminder(pending);
    }
    for (const notification of await notificationManager.getActiveNotifications()) {
      if (notification.id === id) {
        await this.removeDeliveredNotification(id, notification.label ?? '');
      }
    }
    await store.delete(key);
    await store.flush();
  }

  private async removeDeliveredNotification(id: number, label: string): Promise<void> {
    try {
      await notificationManager.cancel(id, label);
    } catch (error) {
      if ((error as BusinessError).code !== 1600007) {
        throw new Error('HuxerUI could not remove the OHOS notification');
      }
    }
  }
}
