#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <catch2/catch_amalgamated.hpp>

#if defined(HUXERUI_TEST_GTK)
#include <gtk/gtk.h>
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  Catch::Session session;
  const int code = session.applyCommandLine(argc, argv);
  if (code != 0) {
    return code;
  }
#if defined(HUXERUI_TEST_GTK)
  // Register GSK types even without a display; display-dependent tests check availability separately.
  static_cast<void>(gtk_init_check());
#endif
  return session.run();
}
