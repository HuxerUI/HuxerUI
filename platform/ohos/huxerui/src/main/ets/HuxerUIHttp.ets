import { url } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';
import { rcp } from '@kit.RemoteCommunicationKit';
import { HuxerUIApplicationModule, HuxerUIHttpHost } from './HuxerUIApplicationModule';

// These event values match the application library transport.
enum HttpEvent { Headers, Body, Progress, Complete, Error }

function requestHeaders(fields: string[][]): rcp.RequestHeaders {
  const headers: rcp.RequestHeaders = {};
  for (const field of fields) {
    const name = field[0].toLowerCase();
    const previous = headers[name];
    if (Array.isArray(previous)) {
      previous.push(field[1]);
    } else {
      headers[name] = previous === undefined ? field[1] : [previous, field[1]];
    }
  }
  return headers;
}

function responseHeaders(headers: rcp.ResponseHeaders): string[][] {
  const result: string[][] = [];
  for (const key of Object.keys(headers)) {
    const value = headers[key];
    if (Array.isArray(value)) {
      for (const item of value) {
        result.push([key, item]);
      }
    } else if (value !== undefined) {
      result.push([key, value]);
    }
  }
  return result;
}

class Request {
  private module: HuxerUIApplicationModule;
  private token: string;
  private address: string;
  private session: rcp.Session;
  private earlyHeaders: rcp.ResponseHeaders | undefined;
  private earlyStatus = 0;
  private terminal = false;
  private headersSent = false;

  constructor(module: HuxerUIApplicationModule, token: string, address: string) {
    this.module = module;
    this.token = token;
    this.address = address;
    this.session = rcp.createSession();
  }

  async start(method: rcp.HttpMethod, fields: string[][], body: ArrayBuffer, streaming: boolean): Promise<void> {
    const request = new rcp.Request(this.address, method, requestHeaders(fields),
      body.byteLength > 0 ? body : undefined);
    const events: rcp.HttpEventsHandler = {
      onUploadProgress: (_total: number, sent: number) => {
        if (!this.terminal) {
          this.module.httpEvent(this.token, HttpEvent.Progress, sent);
        }
      }
    };
    if (streaming) {
      // HarmonyOS 6 exposes the status code separately from the header fields while the body is still open.
      events.onStatusCodeReceive = (status: number) => {
        this.earlyStatus = status;
        this.publishEarlyHeaders();
      };
      events.onHeaderReceive = (headers: rcp.ResponseHeaders) => {
        this.earlyHeaders = headers;
        this.publishEarlyHeaders();
      };
      events.onDataReceive = (data: ArrayBuffer) => this.receiveBody(data);
    }
    request.configuration = { tracing: { httpEventsHandler: events } };
    const response = await this.session.fetch(request);
    if (this.terminal) {
      return;
    }
    if (streaming && !this.headersSent) {
      this.module.httpEvent(this.token, HttpEvent.Error, -3);
      return;
    }
    this.sendHeaders(response.effectiveUrl?.href ?? this.address, response.statusCode, response.headers);
    if (response.body) {
      this.receiveBody(response.body);
    }
    this.module.httpEvent(this.token, HttpEvent.Complete);
  }

  abort(): void {
    if (!this.terminal) {
      this.terminal = true;
      try {
        this.session.close();
      } catch (_) {}
    }
  }

  private publishEarlyHeaders(): void {
    const headers = this.earlyHeaders;
    if (this.terminal || this.headersSent || this.earlyStatus === 0 || !headers) {
      return;
    }
    const status = this.earlyStatus;
    this.earlyStatus = 0;
    this.earlyHeaders = undefined;
    if (status < 200) {
      return;
    }
    if ([301, 302, 303, 307, 308].includes(status) && headers.location) {
      this.address = new url.URL(headers.location, this.address).href;
      return;
    }
    this.sendHeaders(this.address, status, headers);
  }

  private receiveBody(data: ArrayBuffer): void {
    if (!this.terminal && data.byteLength > 0) {
      this.module.httpEvent(this.token, HttpEvent.Body, data);
    }
  }

  private sendHeaders(address: string, status: number, headers: rcp.ResponseHeaders): void {
    if (!this.headersSent && !this.terminal) {
      this.headersSent = true;
      this.module.httpEvent(this.token, HttpEvent.Headers, address, status, responseHeaders(headers));
    }
  }
}

export class HuxerUIHttp implements HuxerUIHttpHost {
  private module: HuxerUIApplicationModule;
  private requests: Map<string, Request> = new Map();

  constructor(module: HuxerUIApplicationModule) {
    this.module = module;
  }

  async start(token: string, address: string, method: string, fields: string[][], body: ArrayBuffer,
    streaming: boolean): Promise<void> {
    try {
      const request = new Request(this.module, token, address);
      this.requests.set(token, request);
      await request.start(method, fields, body, streaming);
    } catch (error) {
      this.module.httpEvent(token, HttpEvent.Error, (error as BusinessError).code ?? -1);
    } finally {
      this.abort(token);
    }
  }

  abort(token: string): void {
    const request = this.requests.get(token);
    this.requests.delete(token);
    request?.abort();
  }
}
