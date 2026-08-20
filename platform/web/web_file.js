Module.preRun ||= [];
Module.preRun.push(() => {
  const dependency = "huxerui-web-file-storage";
  let complete = false;

  const finish = (error) => {
    if (complete) {
      return;
    }
    complete = true;
    if (error) {
      Module.huxerUIFileSystem ||= {};
      Module.huxerUIFileSystem.ready = false;
      Module.huxerUIFileSystem.error = error instanceof Error ? error.message : String(error);
      console.error("HuxerUI Web file storage initialization failed", error);
    }
    removeRunDependency(dependency);
  };

  addRunDependency(dependency);
  try {
    const storageKey = Module.huxeruiStorageKey;
    if (typeof storageKey !== "string" || storageKey.length === 0 || storageKey.length > 256) {
      throw new Error("huxeruiStorageKey must be a non-empty string of at most 256 characters");
    }
    const encodedKey = encodeURIComponent(storageKey);
    if (!encodedKey || encodedKey === "." || encodedKey === "..") {
      throw new Error("huxeruiStorageKey does not produce a valid storage path");
    }

    const persistentRoot = `/huxerui/${encodedKey}`;
    const dataDirectory = `${persistentRoot}/data`;
    const cacheDirectory = `${persistentRoot}/cache`;
    const temporaryDirectory = `/tmp/huxerui/${encodedKey}`;

    FS.mkdirTree(persistentRoot);
    FS.mount(IDBFS, {}, persistentRoot);
    FS.mkdirTree(temporaryDirectory);
    Module.huxerUIFileSystem = {
      ready: false,
      error: "",
      persistentRoot,
      dataDirectory,
      cacheDirectory,
      temporaryDirectory,
    };

    FS.syncfs(true, (restoreError) => {
      if (restoreError) {
        finish(restoreError);
        return;
      }
      try {
        FS.mkdirTree(dataDirectory);
        FS.mkdirTree(cacheDirectory);
      } catch (error) {
        finish(error);
        return;
      }
      try {
        FS.syncfs(false, (persistError) => {
          if (persistError) {
            finish(persistError);
            return;
          }
          Module.huxerUIFileSystem.ready = true;
          finish(null);
        });
      } catch (error) {
        finish(error);
      }
    });
  } catch (error) {
    finish(error);
  }
});
