import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { getNode } from '@ohos/hvigor';
import { appTasks, OhosAppContext, OhosPluginId } from '@ohos/hvigor-ohos-plugin';

getNode(__filename)!.afterNodeEvaluate((node) => {
  const context = node.getContext(OhosPluginId.OHOS_APP_PLUGIN) as OhosAppContext;
  const path = join(context.getProjectPath(), '.huxerui', 'signing.json');
  if (!existsSync(path)) {
    return;
  }

  const signing = JSON.parse(readFileSync(path, 'utf8'));
  if (typeof signing?.name !== 'string' || !signing.name) {
    throw new Error('.huxerui/signing.json must contain a named HarmonyOS signing configuration');
  }
  const profile = context.getBuildProfileOpt();
  const product = profile.app.products?.find((item) => item.name === 'default');
  if (!product) {
    throw new Error('The HuxerUI example runner requires the default product');
  }
  profile.app.signingConfigs = [signing];
  product.signingConfig = signing.name;
  context.setBuildProfileOpt(profile);
});

export default {
  system: appTasks,
  plugins: []
};
