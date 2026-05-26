// IntelliSense-only Kconfig defaults.
//
// ESP-IDF builds provide these via the generated sdkconfig header and/or
// compiler forced-includes. When `build/` is not generated yet, VSCode
// IntelliSense may report undefined CONFIG_* identifiers.
//
// This file is NOT used by the actual ESP-IDF build.
#pragma once

// From project root `sdkconfig` (April 23, 2026):
#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL 3
#endif

#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 100
#endif
