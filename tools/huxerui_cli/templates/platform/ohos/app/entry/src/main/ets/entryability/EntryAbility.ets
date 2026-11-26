import { HuxerUIAbility, HuxerUIApplicationModule } from '@huxerui/huxerui';
import applicationModule from 'lib@TARGET_NAME@.so';

export default class EntryAbility extends HuxerUIAbility {
  protected getApplicationModule(): HuxerUIApplicationModule {
    return applicationModule;
  }
}
