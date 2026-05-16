#pragma once

#define SYSMON_APP_VERSION "1.0.1"

#if __has_include("build_version.generated.h")
#include "build_version.generated.h"
#endif

#ifndef SYSMON_GIT_BRANCH
#define SYSMON_GIT_BRANCH "unknown"
#endif

#ifndef SYSMON_GIT_COMMIT
#define SYSMON_GIT_COMMIT "unknown"
#endif

#ifndef SYSMON_GIT_DIRTY
#define SYSMON_GIT_DIRTY 0
#endif
