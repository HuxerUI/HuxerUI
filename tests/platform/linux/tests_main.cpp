#include <catch2/catch_amalgamated.hpp>
#include <gtk/gtk.h>

#include "macros_internal.h"

int main(int argc, char* argv[]) {
  Catch::Session session;
  const int code = session.applyCommandLine(argc, argv);
  if (code != 0) {
    return code;
  }
  // Register GSK types even without a display; display-dependent tests check availability separately.
  HUXERUI_DISCARD(gtk_init_check());
  return session.run();
}
