// The program entry an mcpp application selects on the Web and Android with `main = { from = "huxerui/platform-entry" }`.
//
// On those platforms the platform layer starts the application: the Web page mounts the module through its exports,
// and Android loads the application's shared library from the Java host. No `main` runs application code there, as in
// the CMake integration, which compiles no entry for them. mcpp links one translation unit as each program's entry,
// so this unit is that entry and defines nothing.
