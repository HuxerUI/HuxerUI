import { ComponentContent, UIContext, window } from '@kit.ArkUI';
import {
  HuxerUIPlatformEvents, HuxerUIPlatformExtensions, HuxerUIPlatformModule, HuxerUIPlatformPayload,
  HuxerUIPlatformPayloadFactory, HuxerUIPlatformResult, HuxerUIPlatformView, HuxerUIPlatformViewFactory,
  HuxerUIWindowPlatformModuleFactory,
} from '@huxerui/huxerui';

class PlatformTimer implements HuxerUIPlatformModule {
  private events: HuxerUIPlatformEvents;
  private payloads: HuxerUIPlatformPayloadFactory;
  private timer: number | undefined;
  private pendingStart: HuxerUIPlatformResult | undefined;

  constructor(events: HuxerUIPlatformEvents, payloads: HuxerUIPlatformPayloadFactory) {
    this.events = events;
    this.payloads = payloads;
  }

  invoke(method: string, args: HuxerUIPlatformPayload, result: HuxerUIPlatformResult): (() => void) | void {
    if (method === 'start') {
      return this.start(Number(args.requireInt64()), result);
    }
    if (method === 'stop') {
      args.requireNull();
      this.stop()?.fail('example/timer-stopped', 'The timer stopped before its first tick');
      result.complete(this.payloads.nullValue());
      return;
    }
    result.fail('example/unknown-method', method);
  }

  dispose(): void {
    this.stop();
  }

  private start(interval: number, result: HuxerUIPlatformResult): (() => void) | void {
    if (interval <= 0 || interval > 2147483647) {
      result.fail('example/timer-interval', 'The timer interval is outside the ArkTS timer range');
      return;
    }
    this.stop()?.fail('example/timer-replaced', 'The timer was replaced by a newer start call');
    this.pendingStart = result;
    let tick = BigInt(0);
    const timer = setInterval(() => {
      if (this.timer !== timer) {
        return;
      }
      const value = this.payloads.int64(++tick);
      // Keep the start request cancellable until the first tick. Later ticks are events only.
      const pending = this.pendingStart;
      this.pendingStart = undefined;
      pending?.complete(value);
      this.events.emit('tick', value);
    }, interval);
    this.timer = timer;
    return () => {
      if (this.timer === timer) {
        this.stop();
      }
    };
  }

  private stop(): HuxerUIPlatformResult | undefined {
    if (this.timer !== undefined) {
      clearInterval(this.timer);
      this.timer = undefined;
    }
    const pending = this.pendingStart;
    this.pendingStart = undefined;
    return pending;
  }
}

class TimerFactory implements HuxerUIWindowPlatformModuleFactory {
  create(owner: window.Window, context: UIContext, options: HuxerUIPlatformPayload, events: HuxerUIPlatformEvents,
    payloads: HuxerUIPlatformPayloadFactory): HuxerUIPlatformModule {
    options.requireNull();
    return new PlatformTimer(events, payloads);
  }
}

class TextFieldArguments {
  text: string;
  changed: (text: string) => void;

  constructor(text: string, changed: (text: string) => void) {
    this.text = text;
    this.changed = changed;
  }
}

@Builder
function platformTextField(args: TextFieldArguments) {
  TextInput({ text: args.text })
    .width('100%')
    .height('100%')
    .onChange((text: string) => args.changed(text));
}

class PlatformTextField implements HuxerUIPlatformView {
  readonly content: ComponentContent<TextFieldArguments>;
  private args: TextFieldArguments;

  constructor(context: UIContext, properties: HuxerUIPlatformPayload, events: HuxerUIPlatformEvents,
    payloads: HuxerUIPlatformPayloadFactory) {
    this.args = new TextFieldArguments(properties.requireString(), (text) => {
      this.args.text = text;
      events.emit('changed', payloads.string(text));
    });
    this.content = new ComponentContent(context, wrapBuilder(platformTextField), this.args);
  }

  update(properties: HuxerUIPlatformPayload): void {
    const text = properties.requireString();
    // Acknowledging an ArkUI edit must not rebuild the input or reset its caret.
    if (this.args.text === text) {
      return;
    }
    this.args = new TextFieldArguments(text, this.args.changed);
    this.content.update(this.args);
  }

  dispose(): void {
    this.content.dispose();
  }
}

class TextFieldFactory implements HuxerUIPlatformViewFactory {
  create(context: UIContext, properties: HuxerUIPlatformPayload, events: HuxerUIPlatformEvents,
    payloads: HuxerUIPlatformPayloadFactory): HuxerUIPlatformView {
    return new PlatformTextField(context, properties, events, payloads);
  }
}

export class ExampleExtensions implements HuxerUIPlatformExtensions {
  windowModule(name: string): HuxerUIWindowPlatformModuleFactory | undefined {
    return name === 'example/Timer' ? new TimerFactory() : undefined;
  }

  view(name: string): HuxerUIPlatformViewFactory | undefined {
    return name === 'example/PlatformTextField' ? new TextFieldFactory() : undefined;
  }
}
