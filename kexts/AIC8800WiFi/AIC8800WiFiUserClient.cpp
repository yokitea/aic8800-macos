#include "AIC8800WiFiUserClient.hpp"

OSDefineMetaClassAndStructors(AIC8800WiFiUserClient, IOUserClient)

const IOExternalMethodDispatch AIC8800WiFiUserClient::sMethods[AIC_WIFI_UC_METHOD_COUNT] = {
    [AIC_WIFI_UC_GET_STATUS] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sGetStatus),
        0U, 0U, 0U, sizeof(aic_wifi_status_t)
    },
    [AIC_WIFI_UC_START_SCAN] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sStartScan),
        0U, sizeof(aic_wifi_scan_req_t), 0U, 0U
    },
    [AIC_WIFI_UC_GET_SCAN_RESULTS] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sGetScanResults),
        0U, 0U, 0U, sizeof(aic_wifi_scan_list_t)
    },
    [AIC_WIFI_UC_CONNECT] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sConnect),
        0U, sizeof(aic_wifi_connect_req_t), 0U, sizeof(aic_wifi_connect_req_t)
    },
    [AIC_WIFI_UC_DISCONNECT] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sDisconnect),
        0U, sizeof(aic_wifi_disconnect_req_t), 0U, 0U
    },
    [AIC_WIFI_UC_POLL_EAPOL] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sPollEapol),
        0U, 0U, 0U, sizeof(aic_wifi_eapol_frame_t)
    },
    [AIC_WIFI_UC_SEND_EAPOL] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sSendEapol),
        0U, sizeof(aic_wifi_eapol_frame_t), 0U, 0U
    },
    [AIC_WIFI_UC_ADD_KEY] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sAddKey),
        0U, sizeof(aic_wifi_key_req_t), 0U, sizeof(aic_wifi_key_req_t)
    },
    [AIC_WIFI_UC_SET_PORT_LINK] = {
        reinterpret_cast<IOExternalMethodAction>(&AIC8800WiFiUserClient::sSetPortAndLink),
        0U, sizeof(aic_wifi_port_req_t), 0U, 0U
    },
};

bool AIC8800WiFiUserClient::initWithTask(task_t owningTask,
                                         void *securityToken,
                                         UInt32 type,
                                         OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityToken, type, properties)) {
        return false;
    }
    if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        IOLog("AIC8800WiFiUserClient: rejected unprivileged open\n");
        return false;
    }
    isAdmin_ = true;
    return true;
}

bool AIC8800WiFiUserClient::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    provider_ = OSDynamicCast(AIC8800WiFi, provider);
    if (!provider_) {
        return false;
    }
    return true;
}

void AIC8800WiFiUserClient::stop(IOService *provider) {
    provider_ = nullptr;
    super::stop(provider);
}

IOReturn AIC8800WiFiUserClient::clientClose() {
    if (!isInactive()) {
        (void)terminate();
    }
    return kIOReturnSuccess;
}

IOReturn AIC8800WiFiUserClient::externalMethod(uint32_t selector,
                                               IOExternalMethodArguments *arguments,
                                               IOExternalMethodDispatch *dispatch,
                                               OSObject *target,
                                               void *reference) {
    if (!isAdmin_ || !provider_ || isInactive() || provider_->isInactive()) {
        return kIOReturnNotAttached;
    }
    if (selector >= AIC_WIFI_UC_METHOD_COUNT) {
        return kIOReturnUnsupported;
    }
    dispatch = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    target   = this;
    return super::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn AIC8800WiFiUserClient::sGetStatus(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlGetStatus(static_cast<aic_wifi_status_t *>(args->structureOutput));
}

IOReturn AIC8800WiFiUserClient::sStartScan(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureInput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlStartScan(static_cast<const aic_wifi_scan_req_t *>(args->structureInput));
}

IOReturn AIC8800WiFiUserClient::sGetScanResults(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlGetScanResults(static_cast<aic_wifi_scan_list_t *>(args->structureOutput));
}

IOReturn AIC8800WiFiUserClient::sConnect(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureInput || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlConnect(static_cast<const aic_wifi_connect_req_t *>(args->structureInput),
                                         static_cast<aic_wifi_connect_req_t *>(args->structureOutput));
}

IOReturn AIC8800WiFiUserClient::sDisconnect(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureInput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlDisconnect(static_cast<const aic_wifi_disconnect_req_t *>(args->structureInput));
}

IOReturn AIC8800WiFiUserClient::sPollEapol(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlPollEapol(static_cast<aic_wifi_eapol_frame_t *>(args->structureOutput));
}

IOReturn AIC8800WiFiUserClient::sSendEapol(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureInput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlSendEapol(static_cast<const aic_wifi_eapol_frame_t *>(args->structureInput));
}

IOReturn AIC8800WiFiUserClient::sAddKey(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureInput || !args->structureOutput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlAddKey(static_cast<const aic_wifi_key_req_t *>(args->structureInput),
                                        static_cast<aic_wifi_key_req_t *>(args->structureOutput));
}

IOReturn AIC8800WiFiUserClient::sSetPortAndLink(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args) {
    (void)ref;
    if (!target || !target->provider_ || !args || !args->structureInput) {
        return kIOReturnBadArgument;
    }
    return target->provider_->ctlSetPortAndLink(static_cast<const aic_wifi_port_req_t *>(args->structureInput));
}
