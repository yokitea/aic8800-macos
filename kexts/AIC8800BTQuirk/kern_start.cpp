#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>
#include <pexpert/pexpert.h>

#include "AIC8800BTQuirk.hpp"

static AIC8800BTQuirkModule gBTQuirk;

static const char *bootargOff[] = {
    "-aicbtoff",
    "-aicoff",
};

static const char *bootargDebug[] = {
    "-aicbtdbg",
};

static const char *bootargBeta[] = {
    "-aicbtbeta",
};

static void aicBtPluginStart() {
    uint32_t offFlag = 0;
    if (PE_parse_boot_argn("aic_off", &offFlag, sizeof(offFlag)) && offFlag != 0) {
        SYSLOG("aicbt", "plugin skipped due to aic_off=1");
        return;
    }
    (void)gBTQuirk.init();
}

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowSafeMode | LiluAPI::AllowInstallerRecovery,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Sonoma,
    KernelVersion::Tahoe,
    aicBtPluginStart
};

