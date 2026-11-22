import { HuxerUIAbility, HuxerUIApplicationModule, HuxerUIPlatformExtensions } from '@huxerui/huxerui';
import applicationModule from 'libhuxerui_example_runner.so';
import { ExampleExtensions } from './ExampleExtensions';

export default class EntryAbility extends HuxerUIAbility {
  protected getApplicationModule(): HuxerUIApplicationModule {
    return applicationModule;
  }

  protected getPlatformExtensions(): HuxerUIPlatformExtensions {
    return new ExampleExtensions();
  }
}
