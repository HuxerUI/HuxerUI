import { common } from '@kit.AbilityKit';
import { NodeContent, UIContext, window } from '@kit.ArkUI';

export interface HuxerUITextInputHost {
  connect(token: string, inputType: number, enterKey: number): void;
  disconnect(): void;
  changeSelection(text: string, start: number, end: number): void;
  updateCursor(left: number, top: number, width: number, height: number): void;
  show(): void;
  hide(): void;
}

export interface HuxerUIPickedFile {
  fd: number;
  name: string;
  uri: string;
  writable: boolean;
  mimeType: string;
}

export interface HuxerUIFilePickerHost {
  readonly canSelectDirectory: boolean;
  openFiles(token: string, multiple: boolean, suffixes: string): void;
  saveFile(token: string, source: string, name: string, suffixes: string): void;
  openDirectory(token: string, writable: boolean): void;
  accessFiles(token: string, operation: string, key: string, uris: string[]): void;
}

export interface HuxerUISystemServicesHost {
  readonly canSchedule: boolean;
  perform(token: string, operation: number, args: string[]): void;
}

export interface HuxerUISystemTaskHost {
  readonly canContinue: boolean;
  readonly canRunOngoing: boolean;
  submit(token: string, identifier: string, ongoing: boolean, title: string, subtitle: string): number;
  release(token: string): number;
  reportProgress(token: string, percent: number): number;
  updatePresentation(token: string, title: string, subtitle: string): number;
  close(): number;
}

export interface HuxerUIBackgroundTaskHost {
  submit(id: number, delay: number, network: boolean, power: boolean): number;
  withdraw(id: number, network: boolean, power: boolean): number;
}

export interface HuxerUINotificationActivation {
  identifier: string;
  payload: string;
}

export interface HuxerUIActivation {
  notification?: HuxerUINotificationActivation;
  url?: string;
  files?: HuxerUIPickedFile[];
}

export interface HuxerUIHttpHost {
  start(token: string, address: string, method: string, fields: string[][], body: ArrayBuffer,
    streaming: boolean): void;
  abort(token: string): void;
}

export interface HuxerUIApplicationModule {
  start(context: common.Context, locale: string, scale: number, services: HuxerUISystemServicesHost,
    http: HuxerUIHttpHost, activation: HuxerUIActivation | undefined, files: HuxerUIFilePickerHost,
    extensions: Object | undefined, systemTasks: HuxerUISystemTaskHost | undefined,
    backgroundTasks?: HuxerUIBackgroundTaskHost): void;
  stop(): void;
  setActive(active: boolean): void;
  setConfiguration(locale: string, scale: number, systemDark: boolean): void;
  attach(content: NodeContent, context: UIContext, scale: number, textInput: HuxerUITextInputHost,
    filePicker: HuxerUIFilePickerHost, systemDark: boolean, setStatusBarBrightness: (light: boolean) => void,
    owner: window.Window): void;
  detach(): void;
  setSystemInsets(top: number, right: number, bottom: number, left: number, windowHeight: number): void;
  handleBack(): boolean;
  scroll(x: number, y: number, deltaX: number, deltaY: number, touchpad: boolean, shift: boolean, control: boolean,
    alt: boolean): boolean;
  editText(token: string, command: number, first: number, second: number, text: string): void;
  readTextContext(token: string, query: number, length: number): string | number | undefined;
  completeOpenFiles(token: string, files: HuxerUIPickedFile[]): void;
  completeSaveFile(token: string, saved: boolean): void;
  completeFileAccess(token: string, files: HuxerUIPickedFile[], status: number, canPersist: boolean): void;
  completeSystemService(token: string, result: number): void;
  systemTaskEvent(token: string, event: number, result: number, message?: string): void;
  prepareBackgroundWork(directory: string, id: number): boolean;
  backgroundWorkEvent(id: number, start: boolean): void;
  readBackgroundWork(): number[] | undefined;
  reconcileBackgroundWork(missing: number[]): void;
  activate(activation: HuxerUIActivation): void;
  httpEvent(token: string, type: number, value?: string | number | ArrayBuffer, status?: number,
    headers?: string[][]): void;
}
