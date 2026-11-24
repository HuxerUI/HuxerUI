import { HuxerUISurface, handleHuxerUIBack } from '@huxerui/huxerui';

@Entry
@Component
struct Index {
  onBackPress(): boolean {
    return handleHuxerUIBack(this.getUIContext());
  }

  build() {
    Column() {
      HuxerUISurface()
    }
    .width('100%')
    .height('100%')
  }
}
