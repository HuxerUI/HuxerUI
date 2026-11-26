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
  const abis = process.env.HUXERUI_OHOS_ABIS?.split(',') ?? options?.abiFilters;
  if (!target || !options || !abis?.length) {
    throw new Error('The HuxerUI application requires a default target and at least one ABI');
  }

  const mode = context.getBuildMode() === 'release' ? 'release' : 'debug';
  const generated = `./.cxx/huxerui-resources/${mode}`;
  const build = join(context.getModulePath(), generated);
  const packages = join(build, 'packages');
  mkdirSync(join(build, 'resources'), { recursive: true });
  options.abiFilters = abis;
  options.arguments = [...(options.arguments ?? [])];
  // Hvigor splits native arguments at spaces. Carry the generated resource path through the environment.
  process.env.HUXERUI_OHOS_RESOURCE_OUTPUT_ROOT = packages;
  if (process.env.OHOS_NATIVE_SDK) {
    const toolchain = join(process.env.OHOS_NATIVE_SDK, 'build/cmake/ohos.toolchain.cmake').replace(/\\/g, '/');
    if (/\s/.test(toolchain)) {
      throw new Error('OHOS_NATIVE_SDK requires a path without spaces for Hvigor');
    }
    options.arguments.push(`-DCMAKE_TOOLCHAIN_FILE=${toolchain}`, '-DOHOS_STL=c++_static');
  }
  for (const key of ['HUXERUI_OPTIMIZATION', 'HUXERUI_ENABLE_LTO', 'HUXERUI_ENABLE_PROFILING']) {
    if (process.env[key] !== undefined) {
      options.arguments.push(`-D${key}=${process.env[key]}`);
    }
  }
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
      cpSync(join(packages, abis[0], 'package'), rawfile, { recursive: true });
    }
  });
});

export default {
  system: hapTasks,
  plugins: []
};
