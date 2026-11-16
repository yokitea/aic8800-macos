#ifndef AIC8800_WIFI_USERCLIENT_HPP
#define AIC8800_WIFI_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "AIC8800WiFi.hpp"

class AIC8800WiFiUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AIC8800WiFiUserClient)

    using super = IOUserClient;

public:
    bool initWithTask(task_t owningTask,
                      void *securityToken,
                      UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target,
                            void *reference) override;

private:
    static IOReturn sGetStatus(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sStartScan(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sGetScanResults(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sConnect(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sDisconnect(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPollEapol(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sSendEapol(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sAddKey(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sSetPortAndLink(AIC8800WiFiUserClient *target, void *ref, IOExternalMethodArguments *args);

    static const IOExternalMethodDispatch sMethods[AIC_WIFI_UC_METHOD_COUNT];

    AIC8800WiFi *provider_ = nullptr;
    bool         isAdmin_  = false;
};

#endif /* AIC8800_WIFI_USERCLIENT_HPP */
