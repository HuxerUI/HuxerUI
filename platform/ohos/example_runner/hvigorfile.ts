import { cpSync, mkdirSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { getNode, hvigor } from '@ohos/hvigor';
import { hapTasks, OhosHapContext, OhosPluginId } from '@ohos/hvigor-ohos-plugin';

const node = getNode(__filename);

hvigor.nodesEvaluated(() => {
  const context = node.getContext(OhosPluginId.OHOS_HAP_PLUGIN) as OhosHapContext;
  const profile = context.getBuildProfileOpt();
  const target = profile.targets?.find((item) => item.name === 'default');
  const options = profile.buildOption?.externalNativeOptions;
  const abi = options?.abiFilters?.[0];
  if (!target || !options || !abi) {
    throw new Error('The HuxerUI example runner requires a default target and at least one ABI');
  }

  const mode = context.getBuildMode() === 'release' ? 'release' : 'debug';
  const generated = `./.cxx/huxerui-resources/${mode}`;
  const build = join(context.getModulePath(), generated);
  const packages = join(build, 'packages');
  mkdirSync(join(build, 'resources'), { recursive: true });
  options.arguments = [...(options.arguments ?? []), `-DHUXERUI_OHOS_RESOURCE_OUTPUT_ROOT=${packages.replace(/\\/g, '/')}`];
  target.resource = {
    directories: [...(target.resource?.directories ?? ['./src/main/resources']), `${generated}/resources`]
  };
  context.setBuildProfileOpt(profile);

  node.registerTask({
    name: 'default@StageHuxerUIResources',
    dependencies: ['default@BuildNativeWithNinja'],
    postDependencies: ['default@ProcessResource'],
    run() {
      // The resource package is ABI-independent. Native builds keep separate outputs and we copy one of them.
      const rawfile = join(build, 'resources', 'rawfile');
      rmSync(rawfile, { force: true, recursive: true });
      cpSync(join(packages, abi, 'package'), rawfile, { recursive: true });
    }
  });
});

export default {
  system: hapTasks,
  plugins: []
};
