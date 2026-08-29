(() => {
  class TimerInstance {
    constructor(events) {
      this.events = events;
      this.generation = 0;
      this.tick = 0n;
      this.interval = undefined;
      this.pendingStart = undefined;
      this.disposed = false;
    }

    invoke(method, argumentsValue, result) {
      switch (method) {
        case "start":
          return this.start(argumentsValue.requireInt64(), result);
        case "stop":
          argumentsValue.requireNull();
          this.stop(result);
          return undefined;
        default:
          result.fail(
            "huxerui/unsupported-method",
            `HuxerUI example timer does not support ${method}`,
            Module.HuxerUI.PlatformPayload.nullValue(),
          );
          return undefined;
      }
    }

    dispose() {
      if (this.disposed) {
        return;
      }
      this.disposed = true;
      ++this.generation;
      this.pendingStart = undefined;
      this.clearInterval();
    }

    start(intervalMilliseconds, result) {
      const Payload = Module.HuxerUI.PlatformPayload;
      if (intervalMilliseconds <= 0n || intervalMilliseconds > 2147483647n) {
        result.fail(
          "example/timer-interval",
          "The Web timer interval is outside the browser timer range",
          Payload.nullValue(),
        );
        return undefined;
      }
      if (this.disposed) {
        result.fail("example/timer-closed", "The Web timer is closed", Payload.nullValue());
        return undefined;
      }

      const replaced = this.pendingStart;
      this.clearInterval();
      const generation = ++this.generation;
      this.tick = 0n;
      this.pendingStart = result;
      this.interval = setInterval(() => this.onTick(generation), Number(intervalMilliseconds));
      if (replaced !== undefined) {
        replaced.fail(
          "example/timer-replaced",
          "The timer was replaced by a newer start call",
          Payload.nullValue(),
        );
      }
      return () => this.cancelStart(generation);
    }

    stop(result) {
      const Payload = Module.HuxerUI.PlatformPayload;
      if (this.disposed) {
        result.fail("example/timer-closed", "The Web timer is closed", Payload.nullValue());
        return;
      }
      ++this.generation;
      const pending = this.pendingStart;
      this.pendingStart = undefined;
      this.clearInterval();
      if (pending !== undefined) {
        pending.fail(
          "example/timer-stopped",
          "The timer stopped before its first tick",
          Payload.nullValue(),
        );
      }
      result.complete(Payload.nullValue());
    }

    cancelStart(generation) {
      if (!this.disposed && this.generation === generation) {
        ++this.generation;
        this.pendingStart = undefined;
        this.clearInterval();
      }
    }

    onTick(generation) {
      if (this.disposed || this.generation !== generation || this.interval === undefined) {
        return;
      }
      const Payload = Module.HuxerUI.PlatformPayload;
      ++this.tick;
      if (this.pendingStart !== undefined) {
        const firstResult = this.pendingStart;
        this.pendingStart = undefined;
        firstResult.complete(Payload.int64(this.tick));
      }
      this.events.emit("tick", Payload.int64(this.tick));
    }

    clearInterval() {
      if (this.interval !== undefined) {
        globalThis.clearInterval(this.interval);
        this.interval = undefined;
      }
    }
  }

  Module.huxeruiExampleTimerFactory = Object.freeze({
    create(options, events) {
      options.requireNull();
      return new TimerInstance(events);
    },
  });
})();
