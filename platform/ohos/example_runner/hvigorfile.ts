import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { getNode, hvigor } from '@ohos/hvigor';
import { hapTasks, OhosAppContext, OhosHapContext, OhosPluginId } from '@ohos/hvigor-ohos-plugin';

const node = getNode(__filename)!;

function runHdc(deviceArgs: string[], args: string[], success: string): void {
  const result = spawnSync('hdc', [...deviceArgs, ...args], { encoding: 'utf8' });
  if (result.error) {
    throw result.error;
  }
  process.stdout.write(result.stdout);
  process.stderr.write(result.stderr);
  // HDC can print an install or launch error and still exit with status zero.
  if (result.status !== 0 || !(result.stdout + result.stderr).includes(success)) {
    throw new Error(`hdc ${args[0]} failed`);
  }
}

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
  const parameters = hvigor.getParameter();
  const example = parameters.getExtParam('huxeruiExample');
  if (example !== undefined) {
    if (!/^[a-z0-9_]+$/.test(example)) {
      throw new Error('huxeruiExample must be a snake-case example directory name');
    }
    options.arguments.push(`-DHUXERUI_EXAMPLE=${example}`);
  }
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

  node.registerTask({
    name: 'runExample',
    dependencies: ['assembleHap'],
    run() {
      const app = hvigor.getRootNode().getContext(OhosPluginId.OHOS_APP_PLUGIN) as OhosAppContext;
      const product = app.getCurrentProduct();
      const productName = product.getProductName();
      const signing = app.getBuildProfileOpt().app.products?.find((item) => item.name === productName)?.signingConfig;
      const module = context.getModuleName();
      const variant = signing ? 'signed' : 'unsigned';
      const hap = join(context.getModulePath(), 'build', productName, 'outputs', target.name,
        `${module}-${target.name}-${variant}.hap`);
      if (!existsSync(hap)) {
        throw new Error(`HarmonyOS HAP is missing: ${hap}`);
      }
      const targetDevice = parameters.getExtParam('device');
      const deviceArgs = targetDevice ? ['-t', targetDevice] : [];
      runHdc(deviceArgs, ['install', '-r', hap], 'msg:install bundle successfully');
      runHdc(deviceArgs, ['shell', 'aa', 'start', '-b', product.getBundleName(), '-m', module, '-a', 'EntryAbility'],
        'start ability successfully');
    }
  });
});

export default {
  system: hapTasks,
  plugins: []
};
