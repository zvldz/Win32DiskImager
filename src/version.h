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
#define APP_VERSION_MINOR 2
#define APP_VERSION_PATCH 2

#define APP_VERSION_STRINGIFY_(x) #x
#define APP_VERSION_STRINGIFY(x) APP_VERSION_STRINGIFY_(x)

#define APP_VERSION                                           \
    APP_VERSION_STRINGIFY(APP_VERSION_MAJOR) "."              \
    APP_VERSION_STRINGIFY(APP_VERSION_MINOR) "."              \
    APP_VERSION_STRINGIFY(APP_VERSION_PATCH)

#endif
