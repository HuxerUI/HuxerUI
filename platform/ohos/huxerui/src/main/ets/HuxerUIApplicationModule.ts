import { NodeContent, UIContext } from '@kit.ArkUI';

export interface HuxerUIApplicationModule {
  start(): void;
  stop(): void;
  setActive(active: boolean): void;
  attach(content: NodeContent, context: UIContext, scale: number): void;
  detach(): void;
  handleBack(): boolean;
}
