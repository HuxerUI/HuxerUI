import { BusinessError } from '@kit.BasicServicesKit';
import { inputMethod } from '@kit.IMEKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { HuxerUIApplicationModule, HuxerUITextInputHost } from './HuxerUIApplicationModule';

// These values are shared with the C++ text input bridge.
enum EditCommand {
  Insert, DeleteLeft, DeleteRight, Select, Preview, FinishPreview, Enter, Move, Extend, Clipboard, KeyboardHidden
}

enum ContextQuery { Left, Right, Cursor }

class TextInputConnection {
  private module: HuxerUIApplicationModule;
  private token: string;

  constructor(module: HuxerUIApplicationModule, token: string) {
    this.module = module;
    this.token = token;
  }

  subscribePreview(controller: inputMethod.InputMethodController): void {
    controller.on('setPreviewText', this.preview);
    controller.on('finishTextPreview', this.finishPreview);
  }

  subscribeEditing(controller: inputMethod.InputMethodController): void {
    controller.on('insertText', this.insert);
    controller.on('deleteLeft', this.deleteLeft);
    controller.on('deleteRight', this.deleteRight);
    controller.on('selectByRange', this.select);
    controller.on('sendFunctionKey', this.enter);
    controller.on('moveCursor', this.move);
    controller.on('selectByMovement', this.extend);
    controller.on('handleExtendAction', this.clipboard);
    controller.on('sendKeyboardStatus', this.keyboardStatus);
    controller.on('getLeftTextOfCursor', this.leftText);
    controller.on('getRightTextOfCursor', this.rightText);
    controller.on('getTextIndexAtCursor', this.cursor);
  }

  unsubscribe(controller: inputMethod.InputMethodController): void {
    controller.off('insertText', this.insert);
    controller.off('deleteLeft', this.deleteLeft);
    controller.off('deleteRight', this.deleteRight);
    controller.off('selectByRange', this.select);
    controller.off('setPreviewText', this.preview);
    controller.off('finishTextPreview', this.finishPreview);
    controller.off('sendFunctionKey', this.enter);
    controller.off('moveCursor', this.move);
    controller.off('selectByMovement', this.extend);
    controller.off('handleExtendAction', this.clipboard);
    controller.off('sendKeyboardStatus', this.keyboardStatus);
    controller.off('getLeftTextOfCursor', this.leftText);
    controller.off('getRightTextOfCursor', this.rightText);
    controller.off('getTextIndexAtCursor', this.cursor);
  }

  private edit(command: EditCommand, first: number = 0, second: number = 0, text: string = ''): void {
    this.module.editText(this.token, command, first, second, text);
  }

  private read(query: ContextQuery, length: number): string {
    const result = this.module.readTextContext(this.token, query, length);
    return typeof result === 'string' ? result : '';
  }

  private insert = (text: string): void => this.edit(EditCommand.Insert, 0, 0, text);
  private deleteLeft = (length: number): void => this.edit(EditCommand.DeleteLeft, length);
  private deleteRight = (length: number): void => this.edit(EditCommand.DeleteRight, length);
  private select = (range: inputMethod.Range): void => this.edit(EditCommand.Select, range.start, range.end);
  private preview = (text: string, range: inputMethod.Range): void =>
    this.edit(EditCommand.Preview, range.start, range.end, text);
  private finishPreview = (): void => this.edit(EditCommand.FinishPreview);
  private enter = (_: inputMethod.FunctionKey): void => this.edit(EditCommand.Enter);
  private move = (direction: inputMethod.Direction): void => this.edit(EditCommand.Move, direction);
  private extend = (movement: inputMethod.Movement): void => this.edit(EditCommand.Extend, movement.direction);
  private clipboard = (action: inputMethod.ExtendAction): void => this.edit(EditCommand.Clipboard, action);
  private keyboardStatus = (status: inputMethod.KeyboardStatus): void => {
    if (status === inputMethod.KeyboardStatus.HIDE) {
      this.edit(EditCommand.KeyboardHidden);
    }
  };
  private leftText = (length: number): string => this.read(ContextQuery.Left, length);
  private rightText = (length: number): string => this.read(ContextQuery.Right, length);
  private cursor = (): number => {
    const result = this.module.readTextContext(this.token, ContextQuery.Cursor, 0);
    return typeof result === 'number' ? result : 0;
  };
}

// The system controller is shared. Serialize operations across surface replacement as well as focus changes.
let operations: Promise<void> = Promise.resolve();

function enqueue(operation: () => Promise<void>): void {
  operations = operations.then(operation).catch((error: BusinessError) => {
    hilog.warn(0xCAFE, 'HuxerUI', 'OHOS input method operation failed (%{public}d)', error.code);
  });
}

export class HuxerUITextInput implements HuxerUITextInputHost {
  private controller: inputMethod.InputMethodController = inputMethod.getController();
  private requested: TextInputConnection | undefined;
  private connected: TextInputConnection | undefined;
  private module: HuxerUIApplicationModule;
  private windowId: number | undefined;

  constructor(module: HuxerUIApplicationModule, windowId: number | undefined) {
    this.module = module;
    this.windowId = windowId;
  }

  connect(token: string, inputType: number, enterKey: number): void {
    const connection = new TextInputConnection(this.module, token);
    this.requested = connection;
    enqueue(async () => {
      if (this.requested !== connection) {
        return;
      }
      const config: inputMethod.TextConfig = {
        inputAttribute: { textInputType: inputType, enterKeyType: enterKey }
      };
      if (this.windowId !== undefined) {
        config.windowId = this.windowId;
      }
      try {
        // Preview support is negotiated at attachment. Editing events require an attached client.
        connection.subscribePreview(this.controller);
        await this.controller.attach(false, config);
        this.connected = connection;
        connection.subscribeEditing(this.controller);
      } catch (error) {
        connection.unsubscribe(this.controller);
        if (this.connected === connection) {
          this.connected = undefined;
          await this.controller.detach();
        }
        hilog.warn(0xCAFE, 'HuxerUI', 'Could not attach the OHOS input method (%{public}d)',
          (error as BusinessError).code);
      }
    });
  }

  disconnect(): void {
    this.requested = undefined;
    enqueue(async () => {
      const connection = this.connected;
      if (connection) {
        this.connected = undefined;
        connection.unsubscribe(this.controller);
        await this.controller.detach();
      }
    });
  }

  changeSelection(text: string, start: number, end: number): void {
    this.run(() => this.controller.changeSelection(text, start, end));
  }

  updateCursor(left: number, top: number, width: number, height: number): void {
    this.run(() => this.controller.updateCursor({ left: left, top: top, width: width, height: height }));
  }

  show(): void {
    this.run(() => this.controller.showTextInput());
  }

  hide(): void {
    this.run(() => this.controller.hideTextInput());
  }

  private run(operation: () => Promise<void>): void {
    const connection = this.requested;
    if (connection) {
      enqueue(async () => {
        if (this.requested === connection && this.connected === connection) {
          await operation();
        }
      });
    }
  }
}
