/**********************************************************************
 * Single source of truth for the application version.
 * Included from C++ sources (main.cpp, cli_main.cpp) and from the
 * resource files (DiskImager.rc, DiskImagerCli.rc). Bumping the three
 * integer macros below propagates to:
 *   - PE FILEVERSION / PRODUCTVERSION fields
 *   - FileVersion / ProductVersion string values in VERSIONINFO
 *   - GUI title bar (main.cpp)
 *   - CLI `--version` output (cli_main.cpp)
 *   - setup.iss (reads PRODUCT_VERSION back out of the built exe)
 **********************************************************************/

#ifndef WDI_VERSION_H
#define WDI_VERSION_H

#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 3
#define APP_VERSION_PATCH 2

// Set by CI when the build is not from a tag push (workflow_dispatch /
// dev artifact builds). Adds a "-dev" SemVer pre-release suffix so the
// running exe reports e.g. "2.3.0-dev" instead of "2.3.0", and the
// update checker can rank a real published 2.3.0 release ABOVE the
// current dev build (SemVer rule: pre-release < release).
//
// WDI_COMMIT_SHA, when also defined (CI sets it to ${GITHUB_SHA::7}),
// appends "-<sha>" so the exact source commit is visible in the running
// binary — version becomes e.g. "2.3.2-dev-b95681c". Lets you confirm
// you actually installed the test build you just kicked off.
#ifdef WDI_DEV_BUILD
#  ifdef WDI_COMMIT_SHA
#    define APP_VERSION_SUFFIX "-dev-" APP_VERSION_STRINGIFY(WDI_COMMIT_SHA)
#  else
#    define APP_VERSION_SUFFIX "-dev"
#  endif
#else
#  define APP_VERSION_SUFFIX ""
#endif

#define APP_VERSION_STRINGIFY_(x) #x
#define APP_VERSION_STRINGIFY(x) APP_VERSION_STRINGIFY_(x)

#define APP_VERSION                                           \
    APP_VERSION_STRINGIFY(APP_VERSION_MAJOR) "."              \
    APP_VERSION_STRINGIFY(APP_VERSION_MINOR) "."              \
    APP_VERSION_STRINGIFY(APP_VERSION_PATCH)                  \
    APP_VERSION_SUFFIX

#endif
