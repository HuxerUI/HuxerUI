import { HuxerUIAbility, HuxerUIApplicationModule } from '@huxerui/huxerui';
import applicationModule from 'libhuxerui_example_runner.so';

export default class EntryAbility extends HuxerUIAbility {
  protected getApplicationModule(): HuxerUIApplicationModule {
    return applicationModule;
  }
}
