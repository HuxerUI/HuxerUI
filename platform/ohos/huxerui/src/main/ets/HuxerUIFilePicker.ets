import { common } from '@kit.AbilityKit';
import { preferences, uniformTypeDescriptor } from '@kit.ArkData';
import { util } from '@kit.ArkTS';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { fileIo, fileShare, picker } from '@kit.CoreFileKit';
import { HuxerUIApplicationModule, HuxerUIFilePickerHost, HuxerUIPickedFile } from './HuxerUIApplicationModule';

// Match the application library's file access result codes.
enum AccessStatus { Success, NotFound, Denied, Unsupported, TooLarge, Failed }

function fileMimeType(name: string): string {
  const dot = name.lastIndexOf('.');
  if (dot > 0 && dot + 1 < name.length) {
    try {
      const type = uniformTypeDescriptor.getUniformDataTypeByFilenameExtension(name.substring(dot).toLowerCase());
      const mimes = uniformTypeDescriptor.getTypeDescriptor(type).mimeTypes;
      return mimes.length > 0 ? mimes[0] : '';
    } catch (_) {}
  }
  return '';
}

function pickedFile(file: fileIo.File, uri: string, writable: boolean): HuxerUIPickedFile {
  return { fd: file.fd, name: file.name, uri: uri, writable: writable, mimeType: fileMimeType(file.name) };
}

export function withGrantedFiles(uris: string[], writable: boolean,
  receive: (files?: HuxerUIPickedFile[]) => void): void {
  const opened: fileIo.File[] = [];
  const files: HuxerUIPickedFile[] = [];
  try {
    try {
      for (const uri of uris) {
        let file: fileIo.File | undefined;
        if (writable) {
          try {
            file = fileIo.openSync(uri, fileIo.OpenMode.READ_WRITE);
          } catch (_) {}
        }
        const canWrite = file !== undefined;
        file = file ?? fileIo.openSync(uri, fileIo.OpenMode.READ_ONLY);
        opened.push(file);
        files.push(pickedFile(file, uri, canWrite));
      }
    } catch (_) {
      files.length = 0;
    }
    // The application library duplicates the descriptors before the ability callback returns.
    receive(files.length > 0 ? files : undefined);
  } finally {
    for (const file of opened) {
      try {
        fileIo.closeSync(file);
      } catch (_) {}
    }
  }
}

export class HuxerUIFilePicker implements HuxerUIFilePickerHost {
  readonly canSelectDirectory = canIUse('SystemCapability.FileManagement.UserFileService.FolderSelection') &&
    (deviceInfo.deviceType !== 'phone' || deviceInfo.sdkApiVersion >= 26);
  private module: HuxerUIApplicationModule;
  private context: common.UIAbilityContext;
  private accessQueue: Promise<void> = Promise.resolve();

  constructor(module: HuxerUIApplicationModule, context: common.UIAbilityContext) {
    this.module = module;
    this.context = context;
  }

  async openFiles(token: string, multiple: boolean, suffixes: string): Promise<void> {
    const options = new picker.DocumentSelectOptions();
    options.maxSelectNumber = multiple ? 500 : 1;
    if (suffixes.length > 0) {
      options.fileSuffixFilters = [suffixes];
    }
    await this.select(token, options, true);
  }

  async saveFile(token: string, source: string, name: string, suffixes: string): Promise<void> {
    let saved = false;
    try {
      const options = new picker.DocumentSaveOptions();
      options.newFileNames = [name];
      if (suffixes.length > 0) {
        options.fileSuffixChoices = [suffixes];
      }
      const uris = await new picker.DocumentViewPicker(this.context).save(options);
      if (uris.length > 0) {
        const file = await fileIo.open(uris[0], fileIo.OpenMode.WRITE_ONLY);
        try {
          await fileIo.copyFile(source, file.fd);
        } finally {
          await fileIo.close(file);
        }
        saved = true;
      }
    } catch (_) {}
    this.module.completeSaveFile(token, saved);
  }

  async openDirectory(token: string, writable: boolean): Promise<void> {
    const options = new picker.DocumentSelectOptions();
    options.selectMode = picker.DocumentSelectMode.FOLDER;
    options.maxSelectNumber = 1;
    await this.select(token, options, writable);
  }

  async accessFiles(token: string, operation: string, key: string, uris: string[]): Promise<void> {
    if (operation === 'open') {
      await this.completeAccess(token, uris);
      return;
    }
    // Serialize changes to remembered keys so a URI grant is revoked only when no other key references it.
    const task = this.accessQueue.then(async (): Promise<void> => {
      await this.completeAccess(token, uris, operation, key);
    });
    // A failed native callback must not prevent later requests from reaching the system.
    this.accessQueue = task.catch(() => {});
    await task;
  }

  private async select(token: string, options: picker.DocumentSelectOptions, writable: boolean): Promise<void> {
    const opened: fileIo.File[] = [];
    const files: HuxerUIPickedFile[] = [];
    try {
      const uris = await new picker.DocumentViewPicker(this.context).select(options);
      await this.openReferences(uris, opened, files, writable, options.selectMode === picker.DocumentSelectMode.FOLDER);
    } catch (_) {
      files.length = 0;
    }
    try {
      // The application library duplicates the descriptors before this callback returns.
      this.module.completeOpenFiles(token, files);
    } finally {
      for (const file of opened) {
        fileIo.closeSync(file);
      }
    }
  }

  private async openReferences(uris: string[], opened: fileIo.File[], files: HuxerUIPickedFile[],
    writable: boolean = false, directory: boolean = false): Promise<void> {
    for (const uri of uris) {
      let file: fileIo.File | undefined;
      if (writable && !directory) {
        try {
          file = await fileIo.open(uri, fileIo.OpenMode.READ_WRITE);
        } catch (_) {}
      }
      const canWrite = file !== undefined || (writable && directory);
      file = file ?? await fileIo.open(uri, fileIo.OpenMode.READ_ONLY);
      opened.push(file);
      files.push(pickedFile(file, uri, canWrite));
    }
  }

  private async completeAccess(token: string, uris: string[], operation: string = 'open',
    key: string = ''): Promise<void> {
    const opened: fileIo.File[] = [];
    const files: HuxerUIPickedFile[] = [];
    const storageKey = 'file:' + key;
    let status = AccessStatus.Success;
    try {
      if (operation === 'open') {
        await this.openReferences(uris, opened, files);
      } else if (!canIUse('SystemCapability.FileManagement.AppFileService.FolderAuthorization')) {
        status = AccessStatus.Unsupported;
      } else if (util.TextEncoder.create().encodeInto(storageKey).length > preferences.MAX_KEY_LENGTH) {
        status = AccessStatus.TooLarge;
      } else {
        const store = await preferences.getPreferences(this.context, { name: 'huxerui_file_access' });
        const previous = await store.get(storageKey, '') as string;
        if (operation === 'remember') {
          await this.remember(store, storageKey, previous, uris[0]);
        } else if (operation === 'forget') {
          if (previous.length > 0) {
            await this.revokeIfUnused(store, storageKey, previous);
            await store.delete(storageKey);
            await store.flush();
          }
        } else if (previous.length === 0) {
          status = AccessStatus.NotFound;
        } else {
          await fileShare.activatePermission([this.readPolicy(previous)]);
          await this.openReferences([previous], opened, files);
        }
      }
    } catch (error) {
      const code = (error as BusinessError).code;
      if (code === 201 || code === 13900001 || code === 13900012) {
        status = AccessStatus.Denied;
      } else if (code === 801) {
        status = AccessStatus.Unsupported;
      } else if (code === 13900002) {
        status = AccessStatus.NotFound;
      } else {
        status = AccessStatus.Failed;
      }
      files.length = 0;
    }
    try {
      this.module.completeFileAccess(token, files, status, operation === 'restore');
    } finally {
      for (const file of opened) {
        fileIo.closeSync(file);
      }
    }
  }

  private readPolicy(uri: string): fileShare.PolicyInfo {
    return { uri: uri, operationMode: fileShare.OperationMode.READ_MODE };
  }

  private async revokeIfUnused(store: preferences.Preferences, key: string, uri: string): Promise<void> {
    const entries = await store.getAll() as Record<string, preferences.ValueType>;
    if (!Object.keys(entries).some((entry: string): boolean => entry !== key && entries[entry] === uri)) {
      const policies = [this.readPolicy(uri)];
      const persisted = await fileShare.checkPersistentPermission(policies);
      if (persisted[0]) {
        await fileShare.revokePermission(policies);
      }
    }
  }

  private async remember(store: preferences.Preferences, key: string, previous: string, uri: string): Promise<void> {
    await fileShare.persistPermission([this.readPolicy(uri)]);
    if (previous === uri) {
      return;
    }
    try {
      await store.put(key, uri);
      await store.flush();
    } catch (_) {
      if (previous.length > 0) {
        await store.put(key, previous);
      } else {
        await store.delete(key);
      }
      await this.revokeIfUnused(store, key, uri);
      throw new Error('HuxerUI could not save the OHOS file grant');
    }
    if (previous.length > 0) {
      await this.revokeIfUnused(store, key, previous);
    }
  }
}
