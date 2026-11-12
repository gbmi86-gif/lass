Lass test suite (run with ./test_all, same as before)
======================================================

Layout (tests/):
  test_common.h    shared includes, TEST/SECTION macros, counters, test entry points
  test_common.c    the pass/fail/total counters
  test_all.c       main(): runs the groups in the same order as before
  test_core.c      security fixes, chmml, registry, cookie jar, smart resume, dynamic segments,
                   speed graph, clipboard, basket, ftp plugin, proxy, http auth
  test_engine.c    download engine: scheduler, batch, launch and stall tests, retry backoff,
                   queue persist, resume state, zero-byte downloads, .tels temp files
  test_network.c   URL/batch extraction, HTTP host and fixup, content length, range safety,
                   command injection, Retry-After, segment policy
  test_storage.c   .tels container, encryption, store and compaction, secure store, legacy migration
  test_system.c    crash report, shutdown brake, settings, i18n, notify, categories, rtmp, sniffer
  test_browser.c   browser extension pipe tests
  test_hls.c       HLS parser, downloads, resume, stop, engine tests
  test_dash.c      DASH plugin, MP4 fragments, manifest rules, full download and resume
  test_httpd.*, test_ftpd.*, test_hls_server.*   loopback servers used by the tests

Adding a test module:
  1. Create tests/test_<area>.c with '#include "test_common.h"' and a function
     void test_<name>(void) { SECTION("..."); TEST("...", cond); ... }
     Helpers that other modules need must be non-static and declared in test_common.h.
  2. Add the file to the test_all target in CMakeLists.txt.
  3. Call test_<name>() from main() in test_all.c, at the place where it should run.
