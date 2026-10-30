/*
 * Copyright (c) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "RILC"

#include <android/hardware/radio/1.1/IRadio.h>
#include <android/hardware/radio/1.1/IRadioResponse.h>
#include <android/hardware/radio/1.1/IRadioIndication.h>
#include <android/hardware/radio/1.1/types.h>
#include <android/hardware/radio/1.2/IRadio.h>
#include <android/hardware/radio/1.2/IRadioResponse.h>
#include <android/hardware/radio/1.2/IRadioIndication.h>
#include <android/hardware/radio/1.2/types.h>
#include <android/hardware/radio/1.3/IRadio.h>
#include <android/hardware/radio/1.3/IRadioResponse.h>
#include <android/hardware/radio/1.3/IRadioIndication.h>
#include <android/hardware/radio/1.3/types.h>
#include <android/hardware/radio/1.4/IRadio.h>
#include <android/hardware/radio/1.4/IRadioResponse.h>
#include <android/hardware/radio/1.4/IRadioIndication.h>
#include <android/hardware/radio/1.4/types.h>
#include <android/hardware/radio/1.5/IRadio.h>
#include <android/hardware/radio/1.5/IRadioResponse.h>
#include <android/hardware/radio/1.5/IRadioIndication.h>
#include <android/hardware/radio/1.5/types.h>
#include <android/hardware/radio/1.6/IRadio.h>
#include <android/hardware/radio/1.6/IRadioResponse.h>
#include <android/hardware/radio/1.6/IRadioIndication.h>
#include <android/hardware/radio/1.6/types.h>

#include <android/hardware/radio/deprecated/1.0/IOemHook.h>

#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <telephony/ril.h>
#include <telephony/ril_mnc.h>
#include <ril_service.h>
#include <hidl/HidlTransportSupport.h>
#include <utils/SystemClock.h>
#include <inttypes.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cutils/properties.h>

#define INVALID_HEX_CHAR 16

// Enable verbose logging
#define VDBG 1

using namespace android::hardware::radio;
using namespace android::hardware::radio::V1_0;
using namespace android::hardware::radio::deprecated::V1_0;
using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;
using ::android::hardware::Return;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_array;
using ::android::hardware::hidl_bitfield;
using ::android::hardware::Void;
using android::CommandInfo;
using android::RequestInfo;
using android::requestToString;
using android::sp;

#define BOOL_TO_INT(x) (x ? 1 : 0)
#define ATOI_NULL_HANDLED(x) (x ? atoi(x) : -1)
#define ATOI_NULL_HANDLED_DEF(x, defaultVal) (x ? atoi(x) : defaultVal)

#if defined(ANDROID_MULTI_SIM)
#define CALL_ONREQUEST(a, b, c, d, e) \
        s_vendorFunctions->onRequest((a), (b), (c), (d), ((RIL_SOCKET_ID)(e)))
#define CALL_ONSTATEREQUEST(a) s_vendorFunctions->onStateRequest((RIL_SOCKET_ID)(a))
#else
#define CALL_ONREQUEST(a, b, c, d, e) s_vendorFunctions->onRequest((a), (b), (c), (d))
#define CALL_ONSTATEREQUEST(a) s_vendorFunctions->onStateRequest()
#endif

#ifdef OEM_HOOK_DISABLED
constexpr bool kOemHookEnabled = false;
#else
constexpr bool kOemHookEnabled = true;
#endif

RIL_RadioFunctions *s_vendorFunctions = NULL;
static CommandInfo *s_commands;

#ifndef SAMSUNG_NEXT_GEN_MODEM
static int legacyNetworkModeBootstrapResponse(int slotId, int responseType, int serial,
        RIL_Errno e, void *response, size_t responseLen);
#endif

// These requests are implemented by a legacy command, but HIDL requires the
// response matching the method that was invoked. Keep immutable command
// descriptors per facade instead of modifying the global command table.
static CommandInfo s_emergencyDialCommand = {
        RIL_REQUEST_DIAL, radio::emergencyDialResponse};
static CommandInfo s_cdmaSmsExpectMoreCommand = {
        RIL_REQUEST_CDMA_SEND_SMS, radio::sendCdmaSmsExpectMoreResponse};
static CommandInfo s_supplySimDepersonalizationCommand = {
        RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION,
        radio::supplySimDepersonalizationResponse};
static CommandInfo s_setPreferredNetworkTypeBitmapCommand = {
        RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, radio::setPreferredNetworkTypeBitmapResponse};
static CommandInfo s_getPreferredNetworkTypeBitmapCommand = {
        RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE, radio::getPreferredNetworkTypeBitmapResponse};
static CommandInfo s_setAllowedNetworkTypesBitmapCommand = {
        RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, radio::setAllowedNetworkTypesBitmapResponse};
static CommandInfo s_getAllowedNetworkTypesBitmapCommand = {
        RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE, radio::getAllowedNetworkTypesBitmapResponse};
#ifndef SAMSUNG_NEXT_GEN_MODEM
static CommandInfo s_enableUiccApplicationsCommand = {
        RIL_REQUEST_RADIO_POWER, radio::enableUiccApplicationsResponse};
static CommandInfo s_legacyNetworkModeBootstrapCommand = {
        RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, legacyNetworkModeBootstrapResponse};
#endif

static uint32_t legacyNetworkTypeToRaf(int networkType);

namespace {

constexpr int kEfIccid = 0x2FE2;
constexpr int kIccReadBinary = 0xB0;

std::mutex sIccidCacheMutex;
std::set<std::pair<int, int32_t>> sPendingIccidReads;
std::map<int, std::string> sCachedIccids;

std::mutex sUiccApplicationsMutex;
std::map<int, bool> sUiccApplicationsEnabled;
#ifndef SAMSUNG_NEXT_GEN_MODEM
std::map<std::pair<int, int32_t>, bool> sPendingUiccApplicationsState;

enum class LegacyNetworkModeBootstrapState {
    NOT_SENT,
    PENDING,
    COMPLETE,
};

std::mutex sLegacyNetworkModeBootstrapMutex;
std::map<int, LegacyNetworkModeBootstrapState> sLegacyNetworkModeBootstrapStates;
#endif

std::mutex sOperatorCacheMutex;
std::map<int, std::string> sRegisteredOperatorNumeric;

std::mutex sDataRadioTechnologyMutex;
std::map<int, int> sDataRadioTechnology;

#ifndef SAMSUNG_NEXT_GEN_MODEM
/*
 * XMM6260/6262/6360 libsec-ril emits several identical legacy network-state
 * indications for one CP event (CS, PS and serving-network notifications).
 * Modern telephony starts a complete service-state poll for every indication,
 * which can turn one CP event into a burst of operator/registration/selection
 * IPC requests.  Keep the first indication immediate and coalesce only the
 * duplicates in the same short burst.  A later CP event is never hidden.
 */
constexpr int64_t kNetworkStateIndicationCoalesceMs = 250;
std::mutex sNetworkStateIndicationMutex;
std::map<int, int64_t> sLastNetworkStateIndicationMs;
#endif

int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decodeEfIccid(const char *hexResponse, std::string *iccid) {
    if (hexResponse == nullptr || iccid == nullptr) return false;

    const size_t length = strlen(hexResponse);
    if (length == 0 || (length & 1) != 0) return false;

    std::string decoded;
    decoded.reserve(length);
    for (size_t i = 0; i < length; i += 2) {
        const int high = hexNibble(hexResponse[i]);
        const int low = hexNibble(hexResponse[i + 1]);
        if (high < 0 || low < 0) return false;

        // EF_ICCID uses swapped-nibble BCD. A trailing 0xF is padding.
        if (low <= 9) decoded.push_back(static_cast<char>('0' + low));
        else if (low != 0xF) return false;
        if (high <= 9) decoded.push_back(static_cast<char>('0' + high));
        else if (high != 0xF) return false;
    }

    // ITU-T E.118 ICCIDs are normally 19 or 20 digits. Keep a small range
    // for legacy/operator variants, while rejecting corrupt SIM_IO payloads.
    if (decoded.size() < 18 || decoded.size() > 22) return false;
    *iccid = std::move(decoded);
    return true;
}

bool isSuccessfulIccIo(const RIL_SIM_IO_Response& response) {
    return response.sw1 == 0x90 || response.sw1 == 0x91 ||
            response.sw1 == 0x9E || response.sw1 == 0x9F;
}

std::string getCachedIccid(int slotId) {
    std::lock_guard<std::mutex> lock(sIccidCacheMutex);
    const auto it = sCachedIccids.find(slotId);
    return it == sCachedIccids.end() ? std::string() : it->second;
}

bool getUiccApplicationsEnabled(int slotId) {
    std::lock_guard<std::mutex> lock(sUiccApplicationsMutex);
    const auto it = sUiccApplicationsEnabled.find(slotId);
    return it == sUiccApplicationsEnabled.end() ? true : it->second;
}

#ifndef SAMSUNG_NEXT_GEN_MODEM
void rememberUiccApplicationsRequest(int slotId, int32_t serial, bool enable) {
    std::lock_guard<std::mutex> lock(sUiccApplicationsMutex);
    sPendingUiccApplicationsState[{slotId, serial}] = enable;
}

void finishUiccApplicationsRequest(int slotId, int32_t serial, bool success) {
    std::lock_guard<std::mutex> lock(sUiccApplicationsMutex);
    const auto key = std::make_pair(slotId, serial);
    const auto it = sPendingUiccApplicationsState.find(key);
    if (it == sPendingUiccApplicationsState.end()) return;
    if (success) sUiccApplicationsEnabled[slotId] = it->second;
    sPendingUiccApplicationsState.erase(it);
}
#endif

bool isValidOperatorNumeric(const char *numeric) {
    if (numeric == nullptr) return false;
    const size_t length = strlen(numeric);
    if (length != 5 && length != 6) return false;
    const bool allDigits = std::all_of(numeric, numeric + length,
            [](char value) { return value >= '0' && value <= '9'; });
    if (!allDigits) return false;
    // libsec-ril reports 00000 while the CP has no serving PLMN. It is a
    // sentinel, not a registered operator, and must never be reused as the
    // HIDL 1.6 registeredPlmn value after a later state transition.
    return std::any_of(numeric, numeric + length,
            [](char value) { return value != '0'; });
}

void cacheOperatorNumeric(int slotId, const char *numeric) {
    std::lock_guard<std::mutex> lock(sOperatorCacheMutex);
    if (!isValidOperatorNumeric(numeric)) {
        sRegisteredOperatorNumeric.erase(slotId);
        return;
    }
    sRegisteredOperatorNumeric[slotId] = numeric;
}

std::string getCachedOperatorNumeric(int slotId) {
    std::lock_guard<std::mutex> lock(sOperatorCacheMutex);
    const auto it = sRegisteredOperatorNumeric.find(slotId);
    return it == sRegisteredOperatorNumeric.end() ? std::string() : it->second;
}

void cacheDataRadioTechnology(int slotId, int rat) {
    std::lock_guard<std::mutex> lock(sDataRadioTechnologyMutex);
    if (rat <= RADIO_TECH_UNKNOWN) {
        sDataRadioTechnology.erase(slotId);
        return;
    }
    sDataRadioTechnology[slotId] = rat;
}

int getCachedDataRadioTechnology(int slotId) {
    std::lock_guard<std::mutex> lock(sDataRadioTechnologyMutex);
    const auto it = sDataRadioTechnology.find(slotId);
    return it == sDataRadioTechnology.end() ? RADIO_TECH_UNKNOWN : it->second;
}

}  // namespace

#ifndef SAMSUNG_NEXT_GEN_MODEM
static int legacyNetworkModeBootstrapResponse(int slotId, int responseType, int serial,
        RIL_Errno e, void *response, size_t responseLen) {
    (void)responseType;
    (void)serial;
    (void)response;
    (void)responseLen;

    std::lock_guard<std::mutex> lock(sLegacyNetworkModeBootstrapMutex);
    sLegacyNetworkModeBootstrapStates[slotId] = e == RIL_E_SUCCESS
            ? LegacyNetworkModeBootstrapState::COMPLETE
            : LegacyNetworkModeBootstrapState::NOT_SENT;
    RLOGI("legacy network-mode bootstrap: slot %d result %d", slotId, e);
    return 0;
}

static void ensureLegacyNetworkModeInitialized(int slotId) {
    {
        std::lock_guard<std::mutex> lock(sLegacyNetworkModeBootstrapMutex);
        const auto it = sLegacyNetworkModeBootstrapStates.find(slotId);
        if (it != sLegacyNetworkModeBootstrapStates.end() &&
                it->second != LegacyNetworkModeBootstrapState::NOT_SENT) {
            return;
        }
        sLegacyNetworkModeBootstrapStates[slotId] =
                LegacyNetworkModeBootstrapState::PENDING;
    }

    /*
     * Android 16 can restore an allowed-network bitmap from its database and
     * decide that it is already applied without ever programming this v7 RIL.
     * XMM6260/6262/6360 then retains an arbitrary CP mode (commonly WCDMA
     * only). The working LineageOS 21 path always sends legacy network type
     * 0, which libsec-ril translates to IPC_NET_MODE_SEL automatic
     * GSM/WCDMA. Prime that real modem setting once before the first
     * registration query; any later user/framework selection still overrides
     * it through the ordinary setPreferred/AllowedNetworkTypes methods.
     */
    int automaticGsmWcdma = PREF_NET_TYPE_GSM_WCDMA;
    constexpr int kInternalSerialBase = -626200;
    RequestInfo *pRI = android::addRequestToList(
            kInternalSerialBase - slotId, slotId, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
    if (pRI == nullptr) {
        std::lock_guard<std::mutex> lock(sLegacyNetworkModeBootstrapMutex);
        sLegacyNetworkModeBootstrapStates[slotId] =
                LegacyNetworkModeBootstrapState::NOT_SENT;
        return;
    }
    pRI->pCI = &s_legacyNetworkModeBootstrapCommand;
    RLOGI("legacy network-mode bootstrap: slot %d automatic GSM/WCDMA", slotId);
    CALL_ONREQUEST(RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
            &automaticGsmWcdma, sizeof(automaticGsmWcdma), pRI, slotId);
}
#endif

/* For older RILs that do not support new commands RIL_REQUEST_VOICE_RADIO_TECH and
   RIL_UNSOL_VOICE_RADIO_TECH_CHANGED messages, decode the voice radio tech from
   radio state message and store it. Every time there is a change in Radio State
   check to see if voice radio tech changes and notify telephony
 */
int voiceRadioTech = -1;

/* For older RILs that do not support new commands RIL_REQUEST_GET_CDMA_SUBSCRIPTION_SOURCE
   and RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED messages, decode the subscription
   source from radio state and store it. Every time there is a change in Radio State
   check to see if subscription source changed and notify telephony
 */
int cdmaSubscriptionSource = -1;

/* For older RILs that do not send RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, decode the
   SIM/RUIM state from radio state and store it. Every time there is a change in Radio State,
   check to see if SIM/RUIM status changed and notify telephony
 */
int simRuimStatus = -1;

struct RadioImpl;
struct OemHookImpl;

#if (SIM_COUNT >= 2)
sp<RadioImpl> radioService[SIM_COUNT];
sp<OemHookImpl> oemHookService[SIM_COUNT];
int64_t nitzTimeReceived[SIM_COUNT];
// counter used for synchronization. It is incremented every time response callbacks are updated.
volatile int32_t mCounterRadio[SIM_COUNT];
volatile int32_t mCounterOemHook[SIM_COUNT];
#else
sp<RadioImpl> radioService[1];
sp<OemHookImpl> oemHookService[1];
int64_t nitzTimeReceived[1];
// counter used for synchronization. It is incremented every time response callbacks are updated.
volatile int32_t mCounterRadio[1];
volatile int32_t mCounterOemHook[1];
#endif

static pthread_rwlock_t radioServiceRwlock = PTHREAD_RWLOCK_INITIALIZER;

#if (SIM_COUNT >= 2)
static pthread_rwlock_t radioServiceRwlock2 = PTHREAD_RWLOCK_INITIALIZER;
#if (SIM_COUNT >= 3)
static pthread_rwlock_t radioServiceRwlock3 = PTHREAD_RWLOCK_INITIALIZER;
#if (SIM_COUNT >= 4)
static pthread_rwlock_t radioServiceRwlock4 = PTHREAD_RWLOCK_INITIALIZER;
#endif
#endif
#endif

void convertRilHardwareConfigListToHal(void *response, size_t responseLen,
        hidl_vec<HardwareConfig>& records);

void convertRilRadioCapabilityToHal(void *response, size_t responseLen, RadioCapability& rc);

void convertRilLceDataInfoToHal(void *response, size_t responseLen, LceDataInfo& lce);

void convertRilSignalStrengthToHal(void *response, size_t responseLen,
        SignalStrength& signalStrength);

void convertRilSignalStrengthToHal_1_6(int slotId, void *response, size_t responseLen,
        V1_6::SignalStrength& signalStrength);

void convertRilDataCallToHal(RIL_Data_Call_Response_v6 *dcResponse,
        SetupDataCallResult& dcResult);

void convertRilDataCallToHal(RIL_Data_Call_Response_v9 *dcResponse,
        SetupDataCallResult& dcResult);

void convertRilDataCallToHal(RIL_Data_Call_Response_v11 *dcResponse,
        SetupDataCallResult& dcResult);

void convertRilDataCallListToHal(void *response, size_t responseLen,
        hidl_vec<SetupDataCallResult>& dcResultList);

void convertRilDataCallToHal_1_6(void *response, size_t responseLen,
        V1_6::SetupDataCallResult& dcResult);

void convertRilDataCallListToHal_1_6(void *response, size_t responseLen,
        hidl_vec<V1_6::SetupDataCallResult>& dcResultList);

void convertRilCellInfoListToHal(void *response, size_t responseLen, hidl_vec<CellInfo>& records);

void convertRilCellInfoListToHal_1_6(int slotId, void *response, size_t responseLen,
        hidl_vec<V1_6::CellInfo>& records);

struct RadioImpl : public V1_6::IRadio {
    int32_t mSlotId;
    sp<IRadioResponse> mRadioResponse;
    sp<IRadioIndication> mRadioIndication;
    sp<V1_1::IRadioResponse> mRadioResponseV1_1;
    sp<V1_1::IRadioIndication> mRadioIndicationV1_1;
    sp<V1_2::IRadioResponse> mRadioResponseV1_2;
    sp<V1_2::IRadioIndication> mRadioIndicationV1_2;
    sp<V1_3::IRadioResponse> mRadioResponseV1_3;
    sp<V1_3::IRadioIndication> mRadioIndicationV1_3;
    sp<V1_4::IRadioResponse> mRadioResponseV1_4;
    sp<V1_4::IRadioIndication> mRadioIndicationV1_4;
    sp<V1_5::IRadioResponse> mRadioResponseV1_5;
    sp<V1_5::IRadioIndication> mRadioIndicationV1_5;
    sp<V1_6::IRadioResponse> mRadioResponseV1_6;
    sp<V1_6::IRadioIndication> mRadioIndicationV1_6;

    Return<void> setResponseFunctions(
            const ::android::sp<IRadioResponse>& radioResponse,
            const ::android::sp<IRadioIndication>& radioIndication);

    Return<void> getIccCardStatus(int32_t serial);

    Return<void> supplyIccPinForApp(int32_t serial, const hidl_string& pin,
            const hidl_string& aid);

    Return<void> supplyIccPukForApp(int32_t serial, const hidl_string& puk,
            const hidl_string& pin, const hidl_string& aid);

    Return<void> supplyIccPin2ForApp(int32_t serial,
            const hidl_string& pin2,
            const hidl_string& aid);

    Return<void> supplyIccPuk2ForApp(int32_t serial, const hidl_string& puk2,
            const hidl_string& pin2, const hidl_string& aid);

    Return<void> changeIccPinForApp(int32_t serial, const hidl_string& oldPin,
            const hidl_string& newPin, const hidl_string& aid);

    Return<void> changeIccPin2ForApp(int32_t serial, const hidl_string& oldPin2,
            const hidl_string& newPin2, const hidl_string& aid);

    Return<void> supplyNetworkDepersonalization(int32_t serial, const hidl_string& netPin);

    Return<void> getCurrentCalls(int32_t serial);

    Return<void> dial(int32_t serial, const Dial& dialInfo);

    Return<void> getImsiForApp(int32_t serial,
            const ::android::hardware::hidl_string& aid);

    Return<void> hangup(int32_t serial, int32_t gsmIndex);

    Return<void> hangupWaitingOrBackground(int32_t serial);

    Return<void> hangupForegroundResumeBackground(int32_t serial);

    Return<void> switchWaitingOrHoldingAndActive(int32_t serial);

    Return<void> conference(int32_t serial);

    Return<void> rejectCall(int32_t serial);

    Return<void> getLastCallFailCause(int32_t serial);

    Return<void> getSignalStrength(int32_t serial);

    Return<void> getVoiceRegistrationState(int32_t serial);

    Return<void> getDataRegistrationState(int32_t serial);

    Return<void> getOperator(int32_t serial);

    Return<void> setRadioPower(int32_t serial, bool on);

    Return<void> sendDtmf(int32_t serial,
            const ::android::hardware::hidl_string& s);

    Return<void> sendSms(int32_t serial, const GsmSmsMessage& message);

    Return<void> sendSMSExpectMore(int32_t serial, const GsmSmsMessage& message);

    Return<void> setupDataCall(int32_t serial,
            RadioTechnology radioTechnology,
            const DataProfileInfo& profileInfo,
            bool modemCognitive,
            bool roamingAllowed,
            bool isRoaming);

    Return<void> iccIOForApp(int32_t serial,
            const IccIo& iccIo);

    Return<void> sendUssd(int32_t serial,
            const ::android::hardware::hidl_string& ussd);

    Return<void> cancelPendingUssd(int32_t serial);

    Return<void> getClir(int32_t serial);

    Return<void> setClir(int32_t serial, int32_t status);

    Return<void> getCallForwardStatus(int32_t serial,
            const CallForwardInfo& callInfo);

    Return<void> setCallForward(int32_t serial,
            const CallForwardInfo& callInfo);

    Return<void> getCallWaiting(int32_t serial, int32_t serviceClass);

    Return<void> setCallWaiting(int32_t serial, bool enable, int32_t serviceClass);

    Return<void> acknowledgeLastIncomingGsmSms(int32_t serial,
            bool success, SmsAcknowledgeFailCause cause);

    Return<void> acceptCall(int32_t serial);

    Return<void> deactivateDataCall(int32_t serial,
            int32_t cid, bool reasonRadioShutDown);

    Return<void> getFacilityLockForApp(int32_t serial,
            const ::android::hardware::hidl_string& facility,
            const ::android::hardware::hidl_string& password,
            int32_t serviceClass,
            const ::android::hardware::hidl_string& appId);

    Return<void> setFacilityLockForApp(int32_t serial,
            const ::android::hardware::hidl_string& facility,
            bool lockState,
            const ::android::hardware::hidl_string& password,
            int32_t serviceClass,
            const ::android::hardware::hidl_string& appId);

    Return<void> setBarringPassword(int32_t serial,
            const ::android::hardware::hidl_string& facility,
            const ::android::hardware::hidl_string& oldPassword,
            const ::android::hardware::hidl_string& newPassword);

    Return<void> getNetworkSelectionMode(int32_t serial);

    Return<void> setNetworkSelectionModeAutomatic(int32_t serial);

    Return<void> setNetworkSelectionModeManual(int32_t serial,
            const ::android::hardware::hidl_string& operatorNumeric);

    Return<void> getAvailableNetworks(int32_t serial);

    Return<void> startNetworkScan(int32_t serial, const V1_1::NetworkScanRequest& request);

    Return<void> stopNetworkScan(int32_t serial);

    Return<void> startDtmf(int32_t serial,
            const ::android::hardware::hidl_string& s);

    Return<void> stopDtmf(int32_t serial);

    Return<void> getBasebandVersion(int32_t serial);

    Return<void> separateConnection(int32_t serial, int32_t gsmIndex);

    Return<void> setMute(int32_t serial, bool enable);

    Return<void> getMute(int32_t serial);

    Return<void> getClip(int32_t serial);

    Return<void> getDataCallList(int32_t serial);

    Return<void> setSuppServiceNotifications(int32_t serial, bool enable);

    Return<void> writeSmsToSim(int32_t serial,
            const SmsWriteArgs& smsWriteArgs);

    Return<void> deleteSmsOnSim(int32_t serial, int32_t index);

    Return<void> setBandMode(int32_t serial, RadioBandMode mode);

    Return<void> getAvailableBandModes(int32_t serial);

    Return<void> sendEnvelope(int32_t serial,
            const ::android::hardware::hidl_string& command);

    Return<void> sendTerminalResponseToSim(int32_t serial,
            const ::android::hardware::hidl_string& commandResponse);

    Return<void> handleStkCallSetupRequestFromSim(int32_t serial, bool accept);

    Return<void> explicitCallTransfer(int32_t serial);

    Return<void> setPreferredNetworkType(int32_t serial, PreferredNetworkType nwType);

    Return<void> getPreferredNetworkType(int32_t serial);

    Return<void> getNeighboringCids(int32_t serial);

    Return<void> setLocationUpdates(int32_t serial, bool enable);

    Return<void> setCdmaSubscriptionSource(int32_t serial,
            CdmaSubscriptionSource cdmaSub);

    Return<void> setCdmaRoamingPreference(int32_t serial, CdmaRoamingType type);

    Return<void> getCdmaRoamingPreference(int32_t serial);

    Return<void> setTTYMode(int32_t serial, TtyMode mode);

    Return<void> getTTYMode(int32_t serial);

    Return<void> setPreferredVoicePrivacy(int32_t serial, bool enable);

    Return<void> getPreferredVoicePrivacy(int32_t serial);

    Return<void> sendCDMAFeatureCode(int32_t serial,
            const ::android::hardware::hidl_string& featureCode);

    Return<void> sendBurstDtmf(int32_t serial,
            const ::android::hardware::hidl_string& dtmf,
            int32_t on,
            int32_t off);

    Return<void> sendCdmaSms(int32_t serial, const CdmaSmsMessage& sms);

    Return<void> acknowledgeLastIncomingCdmaSms(int32_t serial,
            const CdmaSmsAck& smsAck);

    Return<void> getGsmBroadcastConfig(int32_t serial);

    Return<void> setGsmBroadcastConfig(int32_t serial,
            const hidl_vec<GsmBroadcastSmsConfigInfo>& configInfo);

    Return<void> setGsmBroadcastActivation(int32_t serial, bool activate);

    Return<void> getCdmaBroadcastConfig(int32_t serial);

    Return<void> setCdmaBroadcastConfig(int32_t serial,
            const hidl_vec<CdmaBroadcastSmsConfigInfo>& configInfo);

    Return<void> setCdmaBroadcastActivation(int32_t serial, bool activate);

    Return<void> getCDMASubscription(int32_t serial);

    Return<void> writeSmsToRuim(int32_t serial, const CdmaSmsWriteArgs& cdmaSms);

    Return<void> deleteSmsOnRuim(int32_t serial, int32_t index);

    Return<void> getDeviceIdentity(int32_t serial);

    Return<void> exitEmergencyCallbackMode(int32_t serial);

    Return<void> getSmscAddress(int32_t serial);

    Return<void> setSmscAddress(int32_t serial,
            const ::android::hardware::hidl_string& smsc);

    Return<void> reportSmsMemoryStatus(int32_t serial, bool available);

    Return<void> reportStkServiceIsRunning(int32_t serial);

    Return<void> getCdmaSubscriptionSource(int32_t serial);

    Return<void> requestIsimAuthentication(int32_t serial,
            const ::android::hardware::hidl_string& challenge);

    Return<void> acknowledgeIncomingGsmSmsWithPdu(int32_t serial,
            bool success,
            const ::android::hardware::hidl_string& ackPdu);

    Return<void> sendEnvelopeWithStatus(int32_t serial,
            const ::android::hardware::hidl_string& contents);

    Return<void> getVoiceRadioTechnology(int32_t serial);

    Return<void> getCellInfoList(int32_t serial);

    Return<void> setCellInfoListRate(int32_t serial, int32_t rate);

    Return<void> setInitialAttachApn(int32_t serial, const DataProfileInfo& dataProfileInfo,
            bool modemCognitive, bool isRoaming);

    Return<void> getImsRegistrationState(int32_t serial);

    Return<void> sendImsSms(int32_t serial, const ImsSmsMessage& message);

    Return<void> iccTransmitApduBasicChannel(int32_t serial, const SimApdu& message);

    Return<void> iccOpenLogicalChannel(int32_t serial,
            const ::android::hardware::hidl_string& aid, int32_t p2);

    Return<void> iccCloseLogicalChannel(int32_t serial, int32_t channelId);

    Return<void> iccTransmitApduLogicalChannel(int32_t serial, const SimApdu& message);

    Return<void> nvReadItem(int32_t serial, NvItem itemId);

    Return<void> nvWriteItem(int32_t serial, const NvWriteItem& item);

    Return<void> nvWriteCdmaPrl(int32_t serial,
            const ::android::hardware::hidl_vec<uint8_t>& prl);

    Return<void> nvResetConfig(int32_t serial, ResetNvType resetType);

    Return<void> setUiccSubscription(int32_t serial, const SelectUiccSub& uiccSub);

    Return<void> setDataAllowed(int32_t serial, bool allow);

    Return<void> getHardwareConfig(int32_t serial);

    Return<void> requestIccSimAuthentication(int32_t serial,
            int32_t authContext,
            const ::android::hardware::hidl_string& authData,
            const ::android::hardware::hidl_string& aid);

    Return<void> setDataProfile(int32_t serial,
            const ::android::hardware::hidl_vec<DataProfileInfo>& profiles, bool isRoaming);

    Return<void> requestShutdown(int32_t serial);

    Return<void> getRadioCapability(int32_t serial);

    Return<void> setRadioCapability(int32_t serial, const RadioCapability& rc);

    Return<void> startLceService(int32_t serial, int32_t reportInterval, bool pullMode);

    Return<void> stopLceService(int32_t serial);

    Return<void> pullLceData(int32_t serial);

    Return<void> getModemActivityInfo(int32_t serial);

    Return<void> setAllowedCarriers(int32_t serial,
            bool allAllowed,
            const CarrierRestrictions& carriers);

    Return<void> getAllowedCarriers(int32_t serial);

    Return<void> sendDeviceState(int32_t serial, DeviceStateType deviceStateType, bool state);

    Return<void> setIndicationFilter(int32_t serial, int32_t indicationFilter);

    Return<void> startKeepalive(int32_t serial, const V1_1::KeepaliveRequest& keepalive);

    Return<void> stopKeepalive(int32_t serial, int32_t sessionHandle);

    Return<void> setSimCardPower(int32_t serial, bool powerUp);
    Return<void> setSimCardPower_1_1(int32_t serial,
            const V1_1::CardPowerState state);

    Return<void> responseAcknowledgement();

    Return<void> setCarrierInfoForImsiEncryption(int32_t serial,
            const V1_1::ImsiEncryptionInfo& message);


    // HIDL 1.2 compatibility facade
    Return<void> startNetworkScan_1_2(int32_t serial, const V1_2::NetworkScanRequest& request);
    Return<void> setIndicationFilter_1_2(int32_t serial, hidl_bitfield<V1_2::IndicationFilter> indicationFilter);
    Return<void> setSignalStrengthReportingCriteria(int32_t serial, int32_t hysteresisMs, int32_t hysteresisDb,
            const hidl_vec<int32_t>& thresholdsDbm, V1_2::AccessNetwork accessNetwork);
    Return<void> setLinkCapacityReportingCriteria(int32_t serial, int32_t hysteresisMs,
            int32_t hysteresisDlKbps, int32_t hysteresisUlKbps,
            const hidl_vec<int32_t>& thresholdsDownlinkKbps, const hidl_vec<int32_t>& thresholdsUplinkKbps,
            V1_2::AccessNetwork accessNetwork);
    Return<void> setupDataCall_1_2(int32_t serial, V1_2::AccessNetwork accessNetwork,
            const DataProfileInfo& dataProfileInfo, bool modemCognitive, bool roamingAllowed, bool isRoaming,
            V1_2::DataRequestReason reason, const hidl_vec<hidl_string>& addresses,
            const hidl_vec<hidl_string>& dnses);
    Return<void> deactivateDataCall_1_2(int32_t serial, int32_t cid, V1_2::DataRequestReason reason);

    // HIDL 1.3
    Return<void> setSystemSelectionChannels(int32_t serial, bool specifyChannels,
            const hidl_vec<V1_1::RadioAccessSpecifier>& specifiers);
    Return<void> enableModem(int32_t serial, bool on);
    Return<void> getModemStackStatus(int32_t serial);

    // HIDL 1.4
    Return<void> setupDataCall_1_4(int32_t serial, V1_4::AccessNetwork accessNetwork,
            const V1_4::DataProfileInfo& dataProfileInfo, bool roamingAllowed, V1_2::DataRequestReason reason,
            const hidl_vec<hidl_string>& addresses, const hidl_vec<hidl_string>& dnses);
    Return<void> setInitialAttachApn_1_4(int32_t serial, const V1_4::DataProfileInfo& dataProfileInfo);
    Return<void> setDataProfile_1_4(int32_t serial, const hidl_vec<V1_4::DataProfileInfo>& profiles);
    Return<void> emergencyDial(int32_t serial, const Dial& dialInfo, hidl_bitfield<V1_4::EmergencyServiceCategory> categories,
            const hidl_vec<hidl_string>& urns, V1_4::EmergencyCallRouting routing,
            bool hasKnownUserIntentEmergency, bool isTesting);
    Return<void> startNetworkScan_1_4(int32_t serial, const V1_2::NetworkScanRequest& request);
    Return<void> getPreferredNetworkTypeBitmap(int32_t serial);
    Return<void> setPreferredNetworkTypeBitmap(int32_t serial, hidl_bitfield<V1_4::RadioAccessFamily> networkTypeBitmap);
    Return<void> setAllowedCarriers_1_4(int32_t serial, const V1_4::CarrierRestrictionsWithPriority& carriers,
            V1_4::SimLockMultiSimPolicy multiSimPolicy);
    Return<void> getAllowedCarriers_1_4(int32_t serial);
    Return<void> getSignalStrength_1_4(int32_t serial);

    // HIDL 1.5
    Return<void> setSignalStrengthReportingCriteria_1_5(int32_t serial, const V1_5::SignalThresholdInfo& signalThresholdInfo,
            V1_5::AccessNetwork accessNetwork);
    Return<void> setLinkCapacityReportingCriteria_1_5(int32_t serial, int32_t hysteresisMs, int32_t hysteresisDlKbps,
            int32_t hysteresisUlKbps, const hidl_vec<int32_t>& thresholdsDownlinkKbps,
            const hidl_vec<int32_t>& thresholdsUplinkKbps, V1_5::AccessNetwork accessNetwork);
    Return<void> enableUiccApplications(int32_t serial, bool enable);
    Return<void> areUiccApplicationsEnabled(int32_t serial);
    Return<void> setSystemSelectionChannels_1_5(int32_t serial, bool specifyChannels,
            const hidl_vec<V1_5::RadioAccessSpecifier>& specifiers);
    Return<void> startNetworkScan_1_5(int32_t serial, const V1_5::NetworkScanRequest& request);
    Return<void> setupDataCall_1_5(int32_t serial, V1_5::AccessNetwork accessNetwork,
            const V1_5::DataProfileInfo& dataProfileInfo, bool roamingAllowed, V1_2::DataRequestReason reason,
            const hidl_vec<V1_5::LinkAddress>& addresses, const hidl_vec<hidl_string>& dnses);
    Return<void> setInitialAttachApn_1_5(int32_t serial, const V1_5::DataProfileInfo& dataProfileInfo);
    Return<void> setDataProfile_1_5(int32_t serial, const hidl_vec<V1_5::DataProfileInfo>& profiles);
    Return<void> setRadioPower_1_5(int32_t serial, bool powerOn, bool forEmergencyCall, bool preferredForEmergencyCall);
    Return<void> setIndicationFilter_1_5(int32_t serial, hidl_bitfield<V1_5::IndicationFilter> indicationFilter);
    Return<void> getBarringInfo(int32_t serial);
    Return<void> getVoiceRegistrationState_1_5(int32_t serial);
    Return<void> getDataRegistrationState_1_5(int32_t serial);
    Return<void> setNetworkSelectionModeManual_1_5(int32_t serial, const hidl_string& operatorNumeric,
            V1_5::RadioAccessNetworks ran);
    Return<void> sendCdmaSmsExpectMore(int32_t serial, const CdmaSmsMessage& sms);
    Return<void> supplySimDepersonalization(int32_t serial, V1_5::PersoSubstate persoType, const hidl_string& controlKey);

    // HIDL 1.6
    Return<void> setRadioPower_1_6(int32_t serial, bool powerOn, bool forEmergencyCall, bool preferredForEmergencyCall);
    Return<void> getDataCallList_1_6(int32_t serial);
    Return<void> setupDataCall_1_6(int32_t serial, V1_5::AccessNetwork accessNetwork,
            const V1_5::DataProfileInfo& dataProfileInfo, bool roamingAllowed, V1_2::DataRequestReason reason,
            const hidl_vec<V1_5::LinkAddress>& addresses, const hidl_vec<hidl_string>& dnses, int32_t pduSessionId,
            const V1_6::OptionalSliceInfo& sliceInfo, const V1_6::OptionalTrafficDescriptor& trafficDescriptor,
            bool matchAllRuleAllowed);
    Return<void> sendSms_1_6(int32_t serial, const GsmSmsMessage& message);
    Return<void> sendSmsExpectMore_1_6(int32_t serial, const GsmSmsMessage& message);
    Return<void> sendCdmaSms_1_6(int32_t serial, const CdmaSmsMessage& sms);
    Return<void> sendCdmaSmsExpectMore_1_6(int32_t serial, const CdmaSmsMessage& sms);
    Return<void> setSimCardPower_1_6(int32_t serial, V1_1::CardPowerState powerUp);
    Return<void> setNrDualConnectivityState(int32_t serial, V1_6::NrDualConnectivityState nrDualConnectivityState);
    Return<void> isNrDualConnectivityEnabled(int32_t serial);
    Return<void> allocatePduSessionId(int32_t serial);
    Return<void> releasePduSessionId(int32_t serial, int32_t id);
    Return<void> startHandover(int32_t serial, int32_t callId);
    Return<void> cancelHandover(int32_t serial, int32_t callId);
    Return<void> setAllowedNetworkTypesBitmap(uint32_t serial, hidl_bitfield<V1_4::RadioAccessFamily> networkTypeBitmap);
    Return<void> getAllowedNetworkTypesBitmap(int32_t serial);
    Return<void> setDataThrottling(int32_t serial, V1_6::DataThrottlingAction dataThrottlingAction,
            int64_t completionDurationMillis);
    Return<void> emergencyDial_1_6(int32_t serial, const Dial& dialInfo,
            hidl_bitfield<V1_4::EmergencyServiceCategory> categories, const hidl_vec<hidl_string>& urns,
            V1_4::EmergencyCallRouting routing, bool hasKnownUserIntentEmergency, bool isTesting);
    Return<void> getSystemSelectionChannels(int32_t serial);
    Return<void> getCellInfoList_1_6(int32_t serial);
    Return<void> getVoiceRegistrationState_1_6(int32_t serial);
    Return<void> getSignalStrength_1_6(int32_t serial);
    Return<void> getDataRegistrationState_1_6(int32_t serial);
    Return<void> getCurrentCalls_1_6(int32_t serial);
    Return<void> getSlicingConfig(int32_t serial);
    Return<void> setCarrierInfoForImsiEncryption_1_6(int32_t serial, const V1_6::ImsiEncryptionInfo& imsiEncryptionInfo);
    Return<void> getSimPhonebookRecords(int32_t serial);
    Return<void> getSimPhonebookCapacity(int32_t serial);
    Return<void> updateSimPhonebookRecords(int32_t serial, const V1_6::PhonebookRecordInfo& recordInfo);

    void checkReturnStatus(Return<void>& ret);
};

struct OemHookImpl : public IOemHook {
    int32_t mSlotId;
    sp<IOemHookResponse> mOemHookResponse;
    sp<IOemHookIndication> mOemHookIndication;

    Return<void> setResponseFunctions(
            const ::android::sp<IOemHookResponse>& oemHookResponse,
            const ::android::sp<IOemHookIndication>& oemHookIndication);

    Return<void> sendRequestRaw(int32_t serial,
            const ::android::hardware::hidl_vec<uint8_t>& data);

    Return<void> sendRequestStrings(int32_t serial,
            const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& data);
};

void memsetAndFreeStrings(int numPointers, ...) {
    va_list ap;
    va_start(ap, numPointers);
    for (int i = 0; i < numPointers; i++) {
        char *ptr = va_arg(ap, char *);
        if (ptr) {
#ifdef MEMSET_FREED
#define MAX_STRING_LENGTH 4096
            memset(ptr, 0, strnlen(ptr, MAX_STRING_LENGTH));
#endif
            free(ptr);
        }
    }
    va_end(ap);
}

void sendErrorResponse(RequestInfo *pRI, RIL_Errno err) {
    // Complete through the normal RIL path so the request is removed from the
    // pending list and freed. Calling the response function directly leaked the
    // RequestInfo and could later leave stale serials behind.
    RIL_onRequestComplete(pRI, err, NULL, 0);
}

/**
 * Copies over src to dest. If memory allocation fails, responseFunction() is called for the
 * request with error RIL_E_NO_MEMORY. The size() method is used to determine the size of the
 * destination buffer into which the HIDL string is copied. If there is a discrepancy between
 * the string length reported by the size() method, and the length of the string returned by
 * the c_str() method, the function will return false indicating a failure.
 *
 * Returns true on success, and false on failure.
 */
bool copyHidlStringToRil(char **dest, const hidl_string &src, RequestInfo *pRI, bool allowEmpty) {
    size_t len = src.size();
    if (len == 0 && !allowEmpty) {
        *dest = NULL;
        return true;
    }
    *dest = (char *) calloc(len + 1, sizeof(char));
    if (*dest == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }
    if (strlcpy(*dest, src.c_str(), len + 1) >= (len + 1)) {
        RLOGE("Copy of the HIDL string has been truncated, as "
              "the string length reported by size() does not "
              "match the length of string returned by c_str().");
        free(*dest);
        *dest = NULL;
        sendErrorResponse(pRI, RIL_E_INTERNAL_ERR);
        return false;
    }
    return true;
}

bool copyHidlStringToRil(char **dest, const hidl_string &src, RequestInfo *pRI) {
    return copyHidlStringToRil(dest, src, pRI, false);
}

hidl_string convertCharPtrToHidlString(const char *ptr) {
    hidl_string ret;
    if (ptr != NULL) {
        // TODO: replace this with strnlen
        ret.setToExternal(ptr, strlen(ptr));
    }
    return ret;
}

bool dispatchVoid(int serial, int slotId, int request) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }
    CALL_ONREQUEST(request, NULL, 0, pRI, slotId);
    return true;
}

bool dispatchString(int serial, int slotId, int request, const char * str) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    char *pString;
    if (!copyHidlStringToRil(&pString, str, pRI)) {
        return false;
    }

    CALL_ONREQUEST(request, pString, sizeof(char *), pRI, slotId);

    memsetAndFreeStrings(1, pString);
    return true;
}

bool dispatchStrings(int serial, int slotId, int request, bool allowEmpty, int countStrings, ...) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    char **pStrings;
    pStrings = (char **)calloc(countStrings, sizeof(char *));
    if (pStrings == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }
    va_list ap;
    va_start(ap, countStrings);
    for (int i = 0; i < countStrings; i++) {
        const char* str = va_arg(ap, const char *);
        if (!copyHidlStringToRil(&pStrings[i], hidl_string(str), pRI, allowEmpty)) {
            va_end(ap);
            for (int j = 0; j < i; j++) {
                memsetAndFreeStrings(1, pStrings[j]);
            }
            free(pStrings);
            return false;
        }
    }
    va_end(ap);

    CALL_ONREQUEST(request, pStrings, countStrings * sizeof(char *), pRI, slotId);

    if (pStrings != NULL) {
        for (int i = 0 ; i < countStrings ; i++) {
            memsetAndFreeStrings(1, pStrings[i]);
        }

#ifdef MEMSET_FREED
        memset(pStrings, 0, countStrings * sizeof(char *));
#endif
        free(pStrings);
    }
    return true;
}

bool dispatchStrings(int serial, int slotId, int request, const hidl_vec<hidl_string>& data) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    int countStrings = data.size();
    char **pStrings;
    pStrings = (char **)calloc(countStrings, sizeof(char *));
    if (pStrings == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }

    for (int i = 0; i < countStrings; i++) {
        if (!copyHidlStringToRil(&pStrings[i], data[i], pRI)) {
            for (int j = 0; j < i; j++) {
                memsetAndFreeStrings(1, pStrings[j]);
            }
            free(pStrings);
            return false;
        }
    }

    CALL_ONREQUEST(request, pStrings, countStrings * sizeof(char *), pRI, slotId);

    if (pStrings != NULL) {
        for (int i = 0 ; i < countStrings ; i++) {
            memsetAndFreeStrings(1, pStrings[i]);
        }

#ifdef MEMSET_FREED
        memset(pStrings, 0, countStrings * sizeof(char *));
#endif
        free(pStrings);
    }
    return true;
}

bool dispatchInts(int serial, int slotId, int request, int countInts, ...) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    int *pInts = (int *)calloc(countInts, sizeof(int));

    if (pInts == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }
    va_list ap;
    va_start(ap, countInts);
    for (int i = 0; i < countInts; i++) {
        pInts[i] = va_arg(ap, int);
    }
    va_end(ap);

    CALL_ONREQUEST(request, pInts, countInts * sizeof(int), pRI, slotId);

    if (pInts != NULL) {
#ifdef MEMSET_FREED
        memset(pInts, 0, countInts * sizeof(int));
#endif
        free(pInts);
    }
    return true;
}

bool dispatchCallForwardStatus(int serial, int slotId, int request,
                              const CallForwardInfo& callInfo) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    RIL_CallForwardInfo cf;
    cf.status = (int) callInfo.status;
    cf.reason = callInfo.reason;
    cf.serviceClass = callInfo.serviceClass;
    cf.toa = callInfo.toa;
    cf.timeSeconds = callInfo.timeSeconds;

    if (!copyHidlStringToRil(&cf.number, callInfo.number, pRI)) {
        return false;
    }

    CALL_ONREQUEST(request, &cf, sizeof(cf), pRI, slotId);

    memsetAndFreeStrings(1, cf.number);

    return true;
}

bool dispatchRaw(int serial, int slotId, int request, const hidl_vec<uint8_t>& rawBytes) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    const uint8_t *uData = rawBytes.data();

    CALL_ONREQUEST(request, (void *) uData, rawBytes.size(), pRI, slotId);

    return true;
}

bool dispatchIccApdu(int serial, int slotId, int request, const SimApdu& message) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    RIL_SIM_APDU apdu = {};

    apdu.sessionid = message.sessionId;
    apdu.cla = message.cla;
    apdu.instruction = message.instruction;
    apdu.p1 = message.p1;
    apdu.p2 = message.p2;
    apdu.p3 = message.p3;

    if (!copyHidlStringToRil(&apdu.data, message.data, pRI)) {
        return false;
    }

    CALL_ONREQUEST(request, &apdu, sizeof(apdu), pRI, slotId);

    memsetAndFreeStrings(1, apdu.data);

    return true;
}

void checkReturnStatus(int32_t slotId, Return<void>& ret, bool isRadioService) {
    if (ret.isOk() == false) {
        RLOGE("checkReturnStatus: unable to call response/indication callback");
        // Remote process hosting the callbacks must be dead. Reset the callback objects;
        // there's no other recovery to be done here. When the client process is back up, it will
        // call setResponseFunctions()

        // Caller should already hold rdlock, release that first
        // note the current counter to avoid overwriting updates made by another thread before
        // write lock is acquired.
        int counter = isRadioService ? mCounterRadio[slotId] : mCounterOemHook[slotId];
        pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(slotId);
        int ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(ret == 0);

        // acquire wrlock
        ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
        assert(ret == 0);

        // make sure the counter value has not changed
        if (counter == (isRadioService ? mCounterRadio[slotId] : mCounterOemHook[slotId])) {
            if (isRadioService) {
                radioService[slotId]->mRadioResponse = NULL;
                radioService[slotId]->mRadioIndication = NULL;
                radioService[slotId]->mRadioResponseV1_1 = NULL;
                radioService[slotId]->mRadioIndicationV1_1 = NULL;
            } else {
                oemHookService[slotId]->mOemHookResponse = NULL;
                oemHookService[slotId]->mOemHookIndication = NULL;
            }
            isRadioService ? mCounterRadio[slotId]++ : mCounterOemHook[slotId]++;
        } else {
            RLOGE("checkReturnStatus: not resetting responseFunctions as they likely "
                    "got updated on another thread");
        }

        // release wrlock
        ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(ret == 0);

        // Reacquire rdlock
        ret = pthread_rwlock_rdlock(radioServiceRwlockPtr);
        assert(ret == 0);
    }
}

void RadioImpl::checkReturnStatus(Return<void>& ret) {
    ::checkReturnStatus(mSlotId, ret, true);
}

Return<void> RadioImpl::setResponseFunctions(
        const ::android::sp<IRadioResponse>& radioResponseParam,
        const ::android::sp<IRadioIndication>& radioIndicationParam) {
    RLOGD("setResponseFunctions");

    pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(mSlotId);
    int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
    assert(ret == 0);

    mRadioResponse = radioResponseParam;
    mRadioIndication = radioIndicationParam;
    mRadioResponseV1_6 = V1_6::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_6 = V1_6::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_6 == nullptr || mRadioIndicationV1_6 == nullptr) {
        mRadioResponseV1_6 = nullptr;
        mRadioIndicationV1_6 = nullptr;
    }
    mRadioResponseV1_5 = V1_5::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_5 = V1_5::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_5 == nullptr || mRadioIndicationV1_5 == nullptr) {
        mRadioResponseV1_5 = nullptr;
        mRadioIndicationV1_5 = nullptr;
    }
    mRadioResponseV1_4 = V1_4::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_4 = V1_4::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_4 == nullptr || mRadioIndicationV1_4 == nullptr) {
        mRadioResponseV1_4 = nullptr;
        mRadioIndicationV1_4 = nullptr;
    }
    mRadioResponseV1_3 = V1_3::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_3 = V1_3::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_3 == nullptr || mRadioIndicationV1_3 == nullptr) {
        mRadioResponseV1_3 = nullptr;
        mRadioIndicationV1_3 = nullptr;
    }
    mRadioResponseV1_2 = V1_2::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_2 = V1_2::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_2 == nullptr || mRadioIndicationV1_2 == nullptr) {
        mRadioResponseV1_2 = nullptr;
        mRadioIndicationV1_2 = nullptr;
    }
    mRadioResponseV1_1 = V1_1::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_1 = V1_1::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_1 == nullptr || mRadioIndicationV1_1 == nullptr) {
        mRadioResponseV1_1 = nullptr;
        mRadioIndicationV1_1 = nullptr;
    }

    mCounterRadio[mSlotId]++;

    ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
    assert(ret == 0);

    // client is connected. Send initial indications.
    android::onNewCommandConnect((RIL_SOCKET_ID) mSlotId);

    return Void();
}

Return<void> RadioImpl::getIccCardStatus(int32_t serial) {
#if VDBG
    RLOGD("getIccCardStatus: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_SIM_STATUS);
    return Void();
}

Return<void> RadioImpl::supplyIccPinForApp(int32_t serial, const hidl_string& pin,
        const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPinForApp: serial %d", serial);
#endif
#ifdef MODEM_SIM_PIN_FIX
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PIN, false,
            2, pin.c_str(), aid.c_str());
#else
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PIN, true,
            2, pin.c_str(), aid.c_str());
#endif
    return Void();
}

Return<void> RadioImpl::supplyIccPukForApp(int32_t serial, const hidl_string& puk,
                                           const hidl_string& pin, const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPukForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PUK, true,
            3, puk.c_str(), pin.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::supplyIccPin2ForApp(int32_t serial, const hidl_string& pin2,
                                            const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPin2ForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PIN2, true,
            2, pin2.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::supplyIccPuk2ForApp(int32_t serial, const hidl_string& puk2,
                                            const hidl_string& pin2, const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPuk2ForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PUK2, true,
            3, puk2.c_str(), pin2.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::changeIccPinForApp(int32_t serial, const hidl_string& oldPin,
                                           const hidl_string& newPin, const hidl_string& aid) {
#if VDBG
    RLOGD("changeIccPinForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CHANGE_SIM_PIN, true,
            3, oldPin.c_str(), newPin.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::changeIccPin2ForApp(int32_t serial, const hidl_string& oldPin2,
                                            const hidl_string& newPin2, const hidl_string& aid) {
#if VDBG
    RLOGD("changeIccPin2ForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CHANGE_SIM_PIN2, true,
            3, oldPin2.c_str(), newPin2.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::supplyNetworkDepersonalization(int32_t serial,
                                                       const hidl_string& netPin) {
#if VDBG
    RLOGD("supplyNetworkDepersonalization: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION, true,
            1, netPin.c_str());
    return Void();
}

Return<void> RadioImpl::getCurrentCalls(int32_t serial) {
#if VDBG
    RLOGD("getCurrentCalls: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CURRENT_CALLS);
    return Void();
}

Return<void> RadioImpl::dial(int32_t serial, const Dial& dialInfo) {
#if VDBG
    RLOGD("dial: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_DIAL);
    if (pRI == NULL) {
        return Void();
    }
    RIL_Dial dial = {};
    RIL_UUS_Info uusInfo = {};
    int32_t sizeOfDial = sizeof(dial);

    if (!copyHidlStringToRil(&dial.address, dialInfo.address, pRI)) {
        return Void();
    }
    dial.clir = (int) dialInfo.clir;

    if (dialInfo.uusInfo.size() != 0) {
        uusInfo.uusType = (RIL_UUS_Type) dialInfo.uusInfo[0].uusType;
        uusInfo.uusDcs = (RIL_UUS_DCS) dialInfo.uusInfo[0].uusDcs;

        if (dialInfo.uusInfo[0].uusData.size() == 0) {
            uusInfo.uusData = NULL;
            uusInfo.uusLength = 0;
        } else {
            if (!copyHidlStringToRil(&uusInfo.uusData, dialInfo.uusInfo[0].uusData, pRI)) {
                memsetAndFreeStrings(1, dial.address);
                return Void();
            }
            uusInfo.uusLength = dialInfo.uusInfo[0].uusData.size();
        }

        dial.uusInfo = &uusInfo;
    }

    CALL_ONREQUEST(RIL_REQUEST_DIAL, &dial, sizeOfDial, pRI, mSlotId);

    memsetAndFreeStrings(2, dial.address, uusInfo.uusData);

    return Void();
}

Return<void> RadioImpl::getImsiForApp(int32_t serial, const hidl_string& aid) {
#if VDBG
    RLOGD("getImsiForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_GET_IMSI, false,
            1, aid.c_str());
    return Void();
}

Return<void> RadioImpl::hangup(int32_t serial, int32_t gsmIndex) {
#if VDBG
    RLOGD("hangup: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_HANGUP, 1, gsmIndex);
    return Void();
}

Return<void> RadioImpl::hangupWaitingOrBackground(int32_t serial) {
#if VDBG
    RLOGD("hangupWaitingOrBackground: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND);
    return Void();
}

Return<void> RadioImpl::hangupForegroundResumeBackground(int32_t serial) {
#if VDBG
    RLOGD("hangupForegroundResumeBackground: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND);
    return Void();
}

Return<void> RadioImpl::switchWaitingOrHoldingAndActive(int32_t serial) {
#if VDBG
    RLOGD("switchWaitingOrHoldingAndActive: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE);
    return Void();
}

Return<void> RadioImpl::conference(int32_t serial) {
#if VDBG
    RLOGD("conference: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CONFERENCE);
    return Void();
}

Return<void> RadioImpl::rejectCall(int32_t serial) {
#if VDBG
    RLOGD("rejectCall: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_UDUB);
    return Void();
}

Return<void> RadioImpl::getLastCallFailCause(int32_t serial) {
#if VDBG
    RLOGD("getLastCallFailCause: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_LAST_CALL_FAIL_CAUSE);
    return Void();
}

Return<void> RadioImpl::getSignalStrength(int32_t serial) {
#if VDBG
    RLOGD("getSignalStrength: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SIGNAL_STRENGTH);
    return Void();
}

Return<void> RadioImpl::getVoiceRegistrationState(int32_t serial) {
#if VDBG
    RLOGD("getVoiceRegistrationState: serial %d", serial);
#endif
#ifndef SAMSUNG_NEXT_GEN_MODEM
    ensureLegacyNetworkModeInitialized(mSlotId);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_VOICE_REGISTRATION_STATE);
    return Void();
}

Return<void> RadioImpl::getDataRegistrationState(int32_t serial) {
#if VDBG
    RLOGD("getDataRegistrationState: serial %d", serial);
#endif
#ifndef SAMSUNG_NEXT_GEN_MODEM
    ensureLegacyNetworkModeInitialized(mSlotId);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DATA_REGISTRATION_STATE);
    return Void();
}

Return<void> RadioImpl::getOperator(int32_t serial) {
#if VDBG
    RLOGD("getOperator: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_OPERATOR);
    return Void();
}

Return<void> RadioImpl::setRadioPower(int32_t serial, bool on) {
    RLOGD("setRadioPower: serial %d on %d", serial, on);
    dispatchInts(serial, mSlotId, RIL_REQUEST_RADIO_POWER, 1, BOOL_TO_INT(on));
    return Void();
}

Return<void> RadioImpl::sendDtmf(int32_t serial, const hidl_string& s) {
#if VDBG
    RLOGD("sendDtmf: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_DTMF, s.c_str());
    return Void();
}

Return<void> RadioImpl::sendSms(int32_t serial, const GsmSmsMessage& message) {
#if VDBG
    RLOGD("sendSms: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_SEND_SMS, false,
            2, message.smscPdu.c_str(), message.pdu.c_str());
    return Void();
}

Return<void> RadioImpl::sendSMSExpectMore(int32_t serial, const GsmSmsMessage& message) {
#if VDBG
    RLOGD("sendSMSExpectMore: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_SEND_SMS_EXPECT_MORE, false,
            2, message.smscPdu.c_str(), message.pdu.c_str());
    return Void();
}

static bool convertMvnoTypeToString(MvnoType type, char *&str) {
    switch (type) {
        case MvnoType::IMSI:
            str = (char *)"imsi";
            return true;
        case MvnoType::GID:
            str = (char *)"gid";
            return true;
        case MvnoType::SPN:
            str = (char *)"spn";
            return true;
        case MvnoType::NONE:
            str = (char *)"";
            return true;
    }
    return false;
}

Return<void> RadioImpl::setupDataCall(int32_t serial, RadioTechnology radioTechnology,
                                      const DataProfileInfo& dataProfileInfo, bool modemCognitive,
                                      bool roamingAllowed, bool isRoaming) {

#if VDBG
    RLOGD("setupDataCall: serial %d", serial);
#endif

    if (s_vendorFunctions->version >= 4 && s_vendorFunctions->version <= 14) {
        const hidl_string &protocol =
                (isRoaming ? dataProfileInfo.roamingProtocol : dataProfileInfo.protocol);
        RLOGI("setupDataCall: slot %d legacy RAT %d, v7 RAT parameter %d, profile %d, APN '%s'",
                mSlotId, static_cast<int>(radioTechnology),
                static_cast<int>(radioTechnology) + 2,
                static_cast<int>(dataProfileInfo.profileId), dataProfileInfo.apn.c_str());
        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 7,
            std::to_string((int) radioTechnology + 2).c_str(),
            std::to_string((int) dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int) dataProfileInfo.authType).c_str(),
            protocol.c_str());
    } else if (s_vendorFunctions->version >= 15) {
        char *mvnoTypeStr = NULL;
        if (!convertMvnoTypeToString(dataProfileInfo.mvnoType, mvnoTypeStr)) {
            RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                    RIL_REQUEST_SETUP_DATA_CALL);
            if (pRI != NULL) {
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            }
            return Void();
        }
        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 15,
            std::to_string((int) radioTechnology + 2).c_str(),
            std::to_string((int) dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int) dataProfileInfo.authType).c_str(),
            dataProfileInfo.protocol.c_str(),
            dataProfileInfo.roamingProtocol.c_str(),
            std::to_string(dataProfileInfo.supportedApnTypesBitmap).c_str(),
            std::to_string(dataProfileInfo.bearerBitmap).c_str(),
            modemCognitive ? "1" : "0",
            std::to_string(dataProfileInfo.mtu).c_str(),
            mvnoTypeStr,
            dataProfileInfo.mvnoMatchData.c_str(),
            roamingAllowed ? "1" : "0");
    } else {
        RLOGE("Unsupported RIL version %d, min version expected 4", s_vendorFunctions->version);
        RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                RIL_REQUEST_SETUP_DATA_CALL);
        if (pRI != NULL) {
            sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        }
    }
    return Void();
}

Return<void> RadioImpl::iccIOForApp(int32_t serial, const IccIo& iccIo) {
#if VDBG
    RLOGD("iccIOForApp: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SIM_IO);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SIM_IO_v6 rilIccIo = {};
    rilIccIo.command = iccIo.command;
    rilIccIo.fileid = iccIo.fileId;
    if (!copyHidlStringToRil(&rilIccIo.path, iccIo.path, pRI)) {
        return Void();
    }

    rilIccIo.p1 = iccIo.p1;
    rilIccIo.p2 = iccIo.p2;
    rilIccIo.p3 = iccIo.p3;

    if (!copyHidlStringToRil(&rilIccIo.data, iccIo.data, pRI)) {
        memsetAndFreeStrings(1, rilIccIo.path);
        return Void();
    }

    if (!copyHidlStringToRil(&rilIccIo.pin2, iccIo.pin2, pRI)) {
        memsetAndFreeStrings(2, rilIccIo.path, rilIccIo.data);
        return Void();
    }

    if (!copyHidlStringToRil(&rilIccIo.aidPtr, iccIo.aid, pRI)) {
        memsetAndFreeStrings(3, rilIccIo.path, rilIccIo.data, rilIccIo.pin2);
        return Void();
    }

    if (iccIo.command == kIccReadBinary && iccIo.fileId == kEfIccid) {
        std::lock_guard<std::mutex> lock(sIccidCacheMutex);
        sPendingIccidReads.emplace(mSlotId, serial);
    }

    CALL_ONREQUEST(RIL_REQUEST_SIM_IO, &rilIccIo, sizeof(rilIccIo), pRI, mSlotId);

    memsetAndFreeStrings(4, rilIccIo.path, rilIccIo.data, rilIccIo.pin2, rilIccIo.aidPtr);

    return Void();
}

Return<void> RadioImpl::sendUssd(int32_t serial, const hidl_string& ussd) {
#if VDBG
    RLOGD("sendUssd: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_SEND_USSD, ussd.c_str());
    return Void();
}

Return<void> RadioImpl::cancelPendingUssd(int32_t serial) {
#if VDBG
    RLOGD("cancelPendingUssd: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CANCEL_USSD);
    return Void();
}

Return<void> RadioImpl::getClir(int32_t serial) {
#if VDBG
    RLOGD("getClir: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CLIR);
    return Void();
}

Return<void> RadioImpl::setClir(int32_t serial, int32_t status) {
#if VDBG
    RLOGD("setClir: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_CLIR, 1, status);
    return Void();
}

Return<void> RadioImpl::getCallForwardStatus(int32_t serial, const CallForwardInfo& callInfo) {
#if VDBG
    RLOGD("getCallForwardStatus: serial %d", serial);
#endif
    dispatchCallForwardStatus(serial, mSlotId, RIL_REQUEST_QUERY_CALL_FORWARD_STATUS,
            callInfo);
    return Void();
}

Return<void> RadioImpl::setCallForward(int32_t serial, const CallForwardInfo& callInfo) {
#if VDBG
    RLOGD("setCallForward: serial %d", serial);
#endif
    dispatchCallForwardStatus(serial, mSlotId, RIL_REQUEST_SET_CALL_FORWARD,
            callInfo);
    return Void();
}

Return<void> RadioImpl::getCallWaiting(int32_t serial, int32_t serviceClass) {
#if VDBG
    RLOGD("getCallWaiting: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_QUERY_CALL_WAITING, 1, serviceClass);
    return Void();
}

Return<void> RadioImpl::setCallWaiting(int32_t serial, bool enable, int32_t serviceClass) {
#if VDBG
    RLOGD("setCallWaiting: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_CALL_WAITING, 2, BOOL_TO_INT(enable),
            serviceClass);
    return Void();
}

Return<void> RadioImpl::acknowledgeLastIncomingGsmSms(int32_t serial,
                                                      bool success, SmsAcknowledgeFailCause cause) {
#if VDBG
    RLOGD("acknowledgeLastIncomingGsmSms: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SMS_ACKNOWLEDGE, 2, BOOL_TO_INT(success),
            cause);
    return Void();
}

Return<void> RadioImpl::acceptCall(int32_t serial) {
#if VDBG
    RLOGD("acceptCall: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_ANSWER);
    return Void();
}

Return<void> RadioImpl::deactivateDataCall(int32_t serial,
                                           int32_t cid, bool reasonRadioShutDown) {
#if VDBG
    RLOGD("deactivateDataCall: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_DEACTIVATE_DATA_CALL, false,
            2, (std::to_string(cid)).c_str(), reasonRadioShutDown ? "1" : "0");
    return Void();
}

Return<void> RadioImpl::getFacilityLockForApp(int32_t serial, const hidl_string& facility,
                                              const hidl_string& password, int32_t serviceClass,
                                              const hidl_string& appId) {
#if VDBG
    RLOGD("getFacilityLockForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_QUERY_FACILITY_LOCK, true,
            4, facility.c_str(), password.c_str(),
            (std::to_string(serviceClass)).c_str(), appId.c_str());
    return Void();
}

Return<void> RadioImpl::setFacilityLockForApp(int32_t serial, const hidl_string& facility,
                                              bool lockState, const hidl_string& password,
                                              int32_t serviceClass, const hidl_string& appId) {
#if VDBG
    RLOGD("setFacilityLockForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_SET_FACILITY_LOCK, true,
            5, facility.c_str(), lockState ? "1" : "0", password.c_str(),
            (std::to_string(serviceClass)).c_str(), appId.c_str() );
    return Void();
}

Return<void> RadioImpl::setBarringPassword(int32_t serial, const hidl_string& facility,
                                           const hidl_string& oldPassword,
                                           const hidl_string& newPassword) {
#if VDBG
    RLOGD("setBarringPassword: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CHANGE_BARRING_PASSWORD, true,
            3, facility.c_str(), oldPassword.c_str(), newPassword.c_str());
    return Void();
}

Return<void> RadioImpl::getNetworkSelectionMode(int32_t serial) {
#if VDBG
    RLOGD("getNetworkSelectionMode: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE);
    return Void();
}

Return<void> RadioImpl::setNetworkSelectionModeAutomatic(int32_t serial) {
#if VDBG
    RLOGD("setNetworkSelectionModeAutomatic: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC);
    return Void();
}

Return<void> RadioImpl::setNetworkSelectionModeManual(int32_t serial,
                                                      const hidl_string& operatorNumeric) {
#if VDBG
    RLOGD("setNetworkSelectionModeManual: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL,
            operatorNumeric.c_str());
    return Void();
}

Return<void> RadioImpl::getAvailableNetworks(int32_t serial) {
#if VDBG
    RLOGD("getAvailableNetworks: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_AVAILABLE_NETWORKS);
    return Void();
}

Return<void> RadioImpl::startNetworkScan(int32_t serial, const V1_1::NetworkScanRequest& request) {
#if VDBG
    RLOGD("startNetworkScan: serial %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_START_NETWORK_SCAN);
    if (pRI == NULL) {
        return Void();
    }

    if (request.specifiers.size() > MAX_RADIO_ACCESS_NETWORKS) {
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return Void();
    }

    RIL_NetworkScanRequest scan_request = {};

    scan_request.type = (RIL_ScanType) request.type;
    scan_request.interval = request.interval;
    scan_request.specifiers_length = request.specifiers.size();
    for (size_t i = 0; i < request.specifiers.size(); ++i) {
        if (request.specifiers[i].geranBands.size() > MAX_BANDS ||
            request.specifiers[i].utranBands.size() > MAX_BANDS ||
            request.specifiers[i].eutranBands.size() > MAX_BANDS ||
            request.specifiers[i].channels.size() > MAX_CHANNELS) {
            sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            return Void();
        }
        const V1_1::RadioAccessSpecifier& ras_from =
                request.specifiers[i];
        RIL_RadioAccessSpecifier& ras_to = scan_request.specifiers[i];

        ras_to.radio_access_network = (RIL_RadioAccessNetworks) ras_from.radioAccessNetwork;
        ras_to.channels_length = ras_from.channels.size();

        std::copy(ras_from.channels.begin(), ras_from.channels.end(), ras_to.channels);
        const std::vector<uint32_t> * bands = nullptr;
        switch (request.specifiers[i].radioAccessNetwork) {
            case V1_1::RadioAccessNetworks::GERAN:
                ras_to.bands_length = ras_from.geranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.geranBands;
                break;
            case V1_1::RadioAccessNetworks::UTRAN:
                ras_to.bands_length = ras_from.utranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.utranBands;
                break;
            case V1_1::RadioAccessNetworks::EUTRAN:
                ras_to.bands_length = ras_from.eutranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.eutranBands;
                break;
            default:
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                return Void();
        }
        // safe to copy to geran_bands because it's a union member
        for (size_t idx = 0; idx < ras_to.bands_length; ++idx) {
            ras_to.bands.geran_bands[idx] = (RIL_GeranBands) (*bands)[idx];
        }
    }

    CALL_ONREQUEST(RIL_REQUEST_START_NETWORK_SCAN, &scan_request, sizeof(scan_request), pRI,
            mSlotId);

    return Void();
}

Return<void> RadioImpl::stopNetworkScan(int32_t serial) {
#if VDBG
    RLOGD("stopNetworkScan: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_STOP_NETWORK_SCAN);
    return Void();
}

Return<void> RadioImpl::startDtmf(int32_t serial, const hidl_string& s) {
#if VDBG
    RLOGD("startDtmf: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_DTMF_START,
            s.c_str());
    return Void();
}

Return<void> RadioImpl::stopDtmf(int32_t serial) {
#if VDBG
    RLOGD("stopDtmf: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DTMF_STOP);
    return Void();
}

Return<void> RadioImpl::getBasebandVersion(int32_t serial) {
#if VDBG
    RLOGD("getBasebandVersion: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_BASEBAND_VERSION);
    return Void();
}

Return<void> RadioImpl::separateConnection(int32_t serial, int32_t gsmIndex) {
#if VDBG
    RLOGD("separateConnection: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SEPARATE_CONNECTION, 1, gsmIndex);
    return Void();
}

Return<void> RadioImpl::setMute(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setMute: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_MUTE, 1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::getMute(int32_t serial) {
#if VDBG
    RLOGD("getMute: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_MUTE);
    return Void();
}

Return<void> RadioImpl::getClip(int32_t serial) {
#if VDBG
    RLOGD("getClip: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_CLIP);
    return Void();
}

Return<void> RadioImpl::getDataCallList(int32_t serial) {
#if VDBG
    RLOGD("getDataCallList: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DATA_CALL_LIST);
    return Void();
}

Return<void> RadioImpl::setSuppServiceNotifications(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setSuppServiceNotifications: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION, 1,
            BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::writeSmsToSim(int32_t serial, const SmsWriteArgs& smsWriteArgs) {
#if VDBG
    RLOGD("writeSmsToSim: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_WRITE_SMS_TO_SIM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SMS_WriteArgs args;
    args.status = (int) smsWriteArgs.status;

    if (!copyHidlStringToRil(&args.pdu, smsWriteArgs.pdu, pRI)) {
        return Void();
    }

    if (!copyHidlStringToRil(&args.smsc, smsWriteArgs.smsc, pRI)) {
        memsetAndFreeStrings(1, args.pdu);
        return Void();
    }

    CALL_ONREQUEST(RIL_REQUEST_WRITE_SMS_TO_SIM, &args, sizeof(args), pRI, mSlotId);

    memsetAndFreeStrings(2, args.smsc, args.pdu);

    return Void();
}

Return<void> RadioImpl::deleteSmsOnSim(int32_t serial, int32_t index) {
#if VDBG
    RLOGD("deleteSmsOnSim: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_DELETE_SMS_ON_SIM, 1, index);
    return Void();
}

Return<void> RadioImpl::setBandMode(int32_t serial, RadioBandMode mode) {
#if VDBG
    RLOGD("setBandMode: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_BAND_MODE, 1, mode);
    return Void();
}

Return<void> RadioImpl::getAvailableBandModes(int32_t serial) {
#if VDBG
    RLOGD("getAvailableBandModes: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE);
    return Void();
}

Return<void> RadioImpl::sendEnvelope(int32_t serial, const hidl_string& command) {
#if VDBG
    RLOGD("sendEnvelope: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND,
            command.c_str());
    return Void();
}

Return<void> RadioImpl::sendTerminalResponseToSim(int32_t serial,
                                                  const hidl_string& commandResponse) {
#if VDBG
    RLOGD("sendTerminalResponseToSim: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE,
            commandResponse.c_str());
    return Void();
}

Return<void> RadioImpl::handleStkCallSetupRequestFromSim(int32_t serial, bool accept) {
#if VDBG
    RLOGD("handleStkCallSetupRequestFromSim: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM,
            1, BOOL_TO_INT(accept));
    return Void();
}

Return<void> RadioImpl::explicitCallTransfer(int32_t serial) {
#if VDBG
    RLOGD("explicitCallTransfer: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_EXPLICIT_CALL_TRANSFER);
    return Void();
}

Return<void> RadioImpl::setPreferredNetworkType(int32_t serial, PreferredNetworkType nwType) {
#if VDBG
    RLOGD("setPreferredNetworkType: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, 1, nwType);
    return Void();
}

Return<void> RadioImpl::getPreferredNetworkType(int32_t serial) {
#if VDBG
    RLOGD("getPreferredNetworkType: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE);
    return Void();
}

Return<void> RadioImpl::getNeighboringCids(int32_t serial) {
#if VDBG
    RLOGD("getNeighboringCids: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_NEIGHBORING_CELL_IDS);
    return Void();
}

Return<void> RadioImpl::setLocationUpdates(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setLocationUpdates: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_LOCATION_UPDATES, 1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::setCdmaSubscriptionSource(int32_t serial, CdmaSubscriptionSource cdmaSub) {
#if VDBG
    RLOGD("setCdmaSubscriptionSource: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE, 1, cdmaSub);
    return Void();
}

Return<void> RadioImpl::setCdmaRoamingPreference(int32_t serial, CdmaRoamingType type) {
#if VDBG
    RLOGD("setCdmaRoamingPreference: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE, 1, type);
    return Void();
}

Return<void> RadioImpl::getCdmaRoamingPreference(int32_t serial) {
#if VDBG
    RLOGD("getCdmaRoamingPreference: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE);
    return Void();
}

Return<void> RadioImpl::setTTYMode(int32_t serial, TtyMode mode) {
#if VDBG
    RLOGD("setTTYMode: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_TTY_MODE, 1, mode);
    return Void();
}

Return<void> RadioImpl::getTTYMode(int32_t serial) {
#if VDBG
    RLOGD("getTTYMode: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_TTY_MODE);
    return Void();
}

Return<void> RadioImpl::setPreferredVoicePrivacy(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setPreferredVoicePrivacy: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE,
            1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::getPreferredVoicePrivacy(int32_t serial) {
#if VDBG
    RLOGD("getPreferredVoicePrivacy: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE);
    return Void();
}

Return<void> RadioImpl::sendCDMAFeatureCode(int32_t serial, const hidl_string& featureCode) {
#if VDBG
    RLOGD("sendCDMAFeatureCode: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_CDMA_FLASH,
            featureCode.c_str());
    return Void();
}

Return<void> RadioImpl::sendBurstDtmf(int32_t serial, const hidl_string& dtmf, int32_t on,
                                      int32_t off) {
#if VDBG
    RLOGD("sendBurstDtmf: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CDMA_BURST_DTMF, false,
            3, dtmf.c_str(), (std::to_string(on)).c_str(),
            (std::to_string(off)).c_str());
    return Void();
}

void constructCdmaSms(RIL_CDMA_SMS_Message &rcsm, const CdmaSmsMessage& sms) {
    rcsm.uTeleserviceID = sms.teleserviceId;
    rcsm.bIsServicePresent = BOOL_TO_INT(sms.isServicePresent);
    rcsm.uServicecategory = sms.serviceCategory;
    rcsm.sAddress.digit_mode = (RIL_CDMA_SMS_DigitMode) sms.address.digitMode;
    rcsm.sAddress.number_mode = (RIL_CDMA_SMS_NumberMode) sms.address.numberMode;
    rcsm.sAddress.number_type = (RIL_CDMA_SMS_NumberType) sms.address.numberType;
    rcsm.sAddress.number_plan = (RIL_CDMA_SMS_NumberPlan) sms.address.numberPlan;

    rcsm.sAddress.number_of_digits = sms.address.digits.size();
    int digitLimit= MIN((rcsm.sAddress.number_of_digits), RIL_CDMA_SMS_ADDRESS_MAX);
    for (int i = 0; i < digitLimit; i++) {
        rcsm.sAddress.digits[i] = sms.address.digits[i];
    }

    rcsm.sSubAddress.subaddressType = (RIL_CDMA_SMS_SubaddressType) sms.subAddress.subaddressType;
    rcsm.sSubAddress.odd = BOOL_TO_INT(sms.subAddress.odd);

    rcsm.sSubAddress.number_of_digits = sms.subAddress.digits.size();
    digitLimit= MIN((rcsm.sSubAddress.number_of_digits), RIL_CDMA_SMS_SUBADDRESS_MAX);
    for (int i = 0; i < digitLimit; i++) {
        rcsm.sSubAddress.digits[i] = sms.subAddress.digits[i];
    }

    rcsm.uBearerDataLen = sms.bearerData.size();
    digitLimit= MIN((rcsm.uBearerDataLen), RIL_CDMA_SMS_BEARER_DATA_MAX);
    for (int i = 0; i < digitLimit; i++) {
        rcsm.aBearerData[i] = sms.bearerData[i];
    }
}

Return<void> RadioImpl::sendCdmaSms(int32_t serial, const CdmaSmsMessage& sms) {
#if VDBG
    RLOGD("sendCdmaSms: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_CDMA_SEND_SMS);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CDMA_SMS_Message rcsm = {};
    constructCdmaSms(rcsm, sms);

    CALL_ONREQUEST(pRI->pCI->requestNumber, &rcsm, sizeof(rcsm), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::acknowledgeLastIncomingCdmaSms(int32_t serial, const CdmaSmsAck& smsAck) {
#if VDBG
    RLOGD("acknowledgeLastIncomingCdmaSms: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CDMA_SMS_Ack rcsa = {};

    rcsa.uErrorClass = (RIL_CDMA_SMS_ErrorClass) smsAck.errorClass;
    rcsa.uSMSCauseCode = smsAck.smsCauseCode;

    CALL_ONREQUEST(pRI->pCI->requestNumber, &rcsa, sizeof(rcsa), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::getGsmBroadcastConfig(int32_t serial) {
#if VDBG
    RLOGD("getGsmBroadcastConfig: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG);
    return Void();
}

Return<void> RadioImpl::setGsmBroadcastConfig(int32_t serial,
                                              const hidl_vec<GsmBroadcastSmsConfigInfo>&
                                              configInfo) {
#if VDBG
    RLOGD("setGsmBroadcastConfig: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG);
    if (pRI == NULL) {
        return Void();
    }

    int num = configInfo.size();
    RIL_GSM_BroadcastSmsConfigInfo gsmBci[num];
    RIL_GSM_BroadcastSmsConfigInfo *gsmBciPtrs[num];

    for (int i = 0 ; i < num ; i++ ) {
        gsmBciPtrs[i] = &gsmBci[i];
        gsmBci[i].fromServiceId = configInfo[i].fromServiceId;
        gsmBci[i].toServiceId = configInfo[i].toServiceId;
        gsmBci[i].fromCodeScheme = configInfo[i].fromCodeScheme;
        gsmBci[i].toCodeScheme = configInfo[i].toCodeScheme;
        gsmBci[i].selected = BOOL_TO_INT(configInfo[i].selected);
    }

    CALL_ONREQUEST(pRI->pCI->requestNumber, gsmBciPtrs,
            num * sizeof(RIL_GSM_BroadcastSmsConfigInfo *), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::setGsmBroadcastActivation(int32_t serial, bool activate) {
#if VDBG
    RLOGD("setGsmBroadcastActivation: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION,
            1, BOOL_TO_INT(!activate));
    return Void();
}

Return<void> RadioImpl::getCdmaBroadcastConfig(int32_t serial) {
#if VDBG
    RLOGD("getCdmaBroadcastConfig: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_GET_BROADCAST_SMS_CONFIG);
    return Void();
}

Return<void> RadioImpl::setCdmaBroadcastConfig(int32_t serial,
                                               const hidl_vec<CdmaBroadcastSmsConfigInfo>&
                                               configInfo) {
#if VDBG
    RLOGD("setCdmaBroadcastConfig: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_CDMA_SET_BROADCAST_SMS_CONFIG);
    if (pRI == NULL) {
        return Void();
    }

    int num = configInfo.size();
    RIL_CDMA_BroadcastSmsConfigInfo cdmaBci[num];
    RIL_CDMA_BroadcastSmsConfigInfo *cdmaBciPtrs[num];

    for (int i = 0 ; i < num ; i++ ) {
        cdmaBciPtrs[i] = &cdmaBci[i];
        cdmaBci[i].service_category = configInfo[i].serviceCategory;
        cdmaBci[i].language = configInfo[i].language;
        cdmaBci[i].selected = BOOL_TO_INT(configInfo[i].selected);
    }

    CALL_ONREQUEST(pRI->pCI->requestNumber, cdmaBciPtrs,
            num * sizeof(RIL_CDMA_BroadcastSmsConfigInfo *), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::setCdmaBroadcastActivation(int32_t serial, bool activate) {
#if VDBG
    RLOGD("setCdmaBroadcastActivation: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SMS_BROADCAST_ACTIVATION,
            1, BOOL_TO_INT(!activate));
    return Void();
}

Return<void> RadioImpl::getCDMASubscription(int32_t serial) {
#if VDBG
    RLOGD("getCDMASubscription: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_SUBSCRIPTION);
    return Void();
}

Return<void> RadioImpl::writeSmsToRuim(int32_t serial, const CdmaSmsWriteArgs& cdmaSms) {
#if VDBG
    RLOGD("writeSmsToRuim: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CDMA_SMS_WriteArgs rcsw = {};
    rcsw.status = (int) cdmaSms.status;
    constructCdmaSms(rcsw.message, cdmaSms.message);

    CALL_ONREQUEST(pRI->pCI->requestNumber, &rcsw, sizeof(rcsw), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::deleteSmsOnRuim(int32_t serial, int32_t index) {
#if VDBG
    RLOGD("deleteSmsOnRuim: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM, 1, index);
    return Void();
}

Return<void> RadioImpl::getDeviceIdentity(int32_t serial) {
#if VDBG
    RLOGD("getDeviceIdentity: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DEVICE_IDENTITY);
    return Void();
}

Return<void> RadioImpl::exitEmergencyCallbackMode(int32_t serial) {
#if VDBG
    RLOGD("exitEmergencyCallbackMode: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE);
    return Void();
}

Return<void> RadioImpl::getSmscAddress(int32_t serial) {
#if VDBG
    RLOGD("getSmscAddress: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_SMSC_ADDRESS);
    return Void();
}

Return<void> RadioImpl::setSmscAddress(int32_t serial, const hidl_string& smsc) {
#if VDBG
    RLOGD("setSmscAddress: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_SET_SMSC_ADDRESS,
            smsc.c_str());
    return Void();
}

Return<void> RadioImpl::reportSmsMemoryStatus(int32_t serial, bool available) {
#if VDBG
    RLOGD("reportSmsMemoryStatus: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_REPORT_SMS_MEMORY_STATUS, 1,
            BOOL_TO_INT(available));
    return Void();
}

Return<void> RadioImpl::reportStkServiceIsRunning(int32_t serial) {
#if VDBG
    RLOGD("reportStkServiceIsRunning: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING);
    return Void();
}

Return<void> RadioImpl::getCdmaSubscriptionSource(int32_t serial) {
#if VDBG
    RLOGD("getCdmaSubscriptionSource: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE);
    return Void();
}

Return<void> RadioImpl::requestIsimAuthentication(int32_t serial, const hidl_string& challenge) {
#if VDBG
    RLOGD("requestIsimAuthentication: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_ISIM_AUTHENTICATION,
            challenge.c_str());
    return Void();
}

Return<void> RadioImpl::acknowledgeIncomingGsmSmsWithPdu(int32_t serial, bool success,
                                                         const hidl_string& ackPdu) {
#if VDBG
    RLOGD("acknowledgeIncomingGsmSmsWithPdu: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU, false,
            2, success ? "1" : "0", ackPdu.c_str());
    return Void();
}

Return<void> RadioImpl::sendEnvelopeWithStatus(int32_t serial, const hidl_string& contents) {
#if VDBG
    RLOGD("sendEnvelopeWithStatus: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS,
            contents.c_str());
    return Void();
}

Return<void> RadioImpl::getVoiceRadioTechnology(int32_t serial) {
#if VDBG
    RLOGD("getVoiceRadioTechnology: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_VOICE_RADIO_TECH);
    return Void();
}

Return<void> RadioImpl::getCellInfoList(int32_t serial) {
#if VDBG
    RLOGD("getCellInfoList: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CELL_INFO_LIST);
    return Void();
}

Return<void> RadioImpl::setCellInfoListRate(int32_t serial, int32_t rate) {
#if VDBG
    RLOGD("setCellInfoListRate: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE, 1, rate);
    return Void();
}

Return<void> RadioImpl::setInitialAttachApn(int32_t serial, const DataProfileInfo& dataProfileInfo,
                                            bool modemCognitive, bool isRoaming) {
#if VDBG
    RLOGD("setInitialAttachApn: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_INITIAL_ATTACH_APN);
    if (pRI == NULL) {
        return Void();
    }

    if (s_vendorFunctions->version <= 14) {
        RIL_InitialAttachApn iaa = {};

        if (!copyHidlStringToRil(&iaa.apn, dataProfileInfo.apn, pRI, true)) {
            return Void();
        }

        const hidl_string &protocol =
                (isRoaming ? dataProfileInfo.roamingProtocol : dataProfileInfo.protocol);

        if (!copyHidlStringToRil(&iaa.protocol, protocol, pRI)) {
            memsetAndFreeStrings(1, iaa.apn);
            return Void();
        }
        iaa.authtype = (int) dataProfileInfo.authType;
        if (!copyHidlStringToRil(&iaa.username, dataProfileInfo.user, pRI)) {
            memsetAndFreeStrings(2, iaa.apn, iaa.protocol);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa.password, dataProfileInfo.password, pRI)) {
            memsetAndFreeStrings(3, iaa.apn, iaa.protocol, iaa.username);
            return Void();
        }

#ifdef NEEDS_ROAMING_PROTOCOL_FIELD
        if (!copyHidlStringToRil(&iaa.roamingProtocol, dataProfileInfo.roamingProtocol, pRI)) {
            memsetAndFreeStrings(4, iaa.apn, iaa.protocol, iaa.username, iaa.password);
            return Void();
        }
#endif

#ifdef NEEDS_IMS_TYPE_FIELD
        iaa.imsType = 0;
#endif

        CALL_ONREQUEST(RIL_REQUEST_SET_INITIAL_ATTACH_APN, &iaa, sizeof(iaa), pRI, mSlotId);

#ifdef NEEDS_ROAMING_PROTOCOL_FIELD
        memsetAndFreeStrings(5, iaa.apn, iaa.protocol, iaa.username, iaa.password,
                iaa.roamingProtocol);
#else
        memsetAndFreeStrings(4, iaa.apn, iaa.protocol, iaa.username, iaa.password);
#endif
    } else {
        RIL_InitialAttachApn_v15 iaa = {};

        if (!copyHidlStringToRil(&iaa.apn, dataProfileInfo.apn, pRI, true)) {
            return Void();
        }

        if (!copyHidlStringToRil(&iaa.protocol, dataProfileInfo.protocol, pRI)) {
            memsetAndFreeStrings(1, iaa.apn);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa.roamingProtocol, dataProfileInfo.roamingProtocol, pRI)) {
            memsetAndFreeStrings(2, iaa.apn, iaa.protocol);
            return Void();
        }
        iaa.authtype = (int) dataProfileInfo.authType;
        if (!copyHidlStringToRil(&iaa.username, dataProfileInfo.user, pRI)) {
            memsetAndFreeStrings(3, iaa.apn, iaa.protocol, iaa.roamingProtocol);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa.password, dataProfileInfo.password, pRI)) {
            memsetAndFreeStrings(4, iaa.apn, iaa.protocol, iaa.roamingProtocol, iaa.username);
            return Void();
        }
        iaa.supportedTypesBitmask = dataProfileInfo.supportedApnTypesBitmap;
        iaa.bearerBitmask = dataProfileInfo.bearerBitmap;
        iaa.modemCognitive = BOOL_TO_INT(modemCognitive);
        iaa.mtu = dataProfileInfo.mtu;

        if (!convertMvnoTypeToString(dataProfileInfo.mvnoType, iaa.mvnoType)) {
            sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            memsetAndFreeStrings(5, iaa.apn, iaa.protocol, iaa.roamingProtocol, iaa.username,
                    iaa.password);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa.mvnoMatchData, dataProfileInfo.mvnoMatchData, pRI)) {
            memsetAndFreeStrings(5, iaa.apn, iaa.protocol, iaa.roamingProtocol, iaa.username,
                    iaa.password);
            return Void();
        }

        CALL_ONREQUEST(RIL_REQUEST_SET_INITIAL_ATTACH_APN, &iaa, sizeof(iaa), pRI, mSlotId);

        memsetAndFreeStrings(6, iaa.apn, iaa.protocol, iaa.roamingProtocol, iaa.username,
                iaa.password, iaa.mvnoMatchData);
    }

    return Void();
}

Return<void> RadioImpl::getImsRegistrationState(int32_t serial) {
#if VDBG
    RLOGD("getImsRegistrationState: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_IMS_REGISTRATION_STATE);
    return Void();
}

bool dispatchImsGsmSms(const ImsSmsMessage& message, RequestInfo *pRI) {
    RIL_IMS_SMS_Message rism = {};
    char **pStrings;
    int countStrings = 2;
    int dataLen = sizeof(char *) * countStrings;

    rism.tech = RADIO_TECH_3GPP;
    rism.retry = BOOL_TO_INT(message.retry);
    rism.messageRef = message.messageRef;

    if (message.gsmMessage.size() != 1) {
        RLOGE("dispatchImsGsmSms: Invalid len %s", requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return false;
    }

    pStrings = (char **)calloc(countStrings, sizeof(char *));
    if (pStrings == NULL) {
        RLOGE("dispatchImsGsmSms: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }

    if (!copyHidlStringToRil(&pStrings[0], message.gsmMessage[0].smscPdu, pRI)) {
#ifdef MEMSET_FREED
        memset(pStrings, 0, dataLen);
#endif
        free(pStrings);
        return false;
    }

    if (!copyHidlStringToRil(&pStrings[1], message.gsmMessage[0].pdu, pRI)) {
        memsetAndFreeStrings(1, pStrings[0]);
#ifdef MEMSET_FREED
        memset(pStrings, 0, dataLen);
#endif
        free(pStrings);
        return false;
    }

    rism.message.gsmMessage = pStrings;
    CALL_ONREQUEST(pRI->pCI->requestNumber, &rism, sizeof(RIL_RadioTechnologyFamily) +
            sizeof(uint8_t) + sizeof(int32_t) + dataLen, pRI, pRI->socket_id);

    for (int i = 0 ; i < countStrings ; i++) {
        memsetAndFreeStrings(1, pStrings[i]);
    }

#ifdef MEMSET_FREED
    memset(pStrings, 0, dataLen);
#endif
    free(pStrings);

    return true;
}

struct ImsCdmaSms {
    RIL_IMS_SMS_Message imsSms;
    RIL_CDMA_SMS_Message cdmaSms;
};

bool dispatchImsCdmaSms(const ImsSmsMessage& message, RequestInfo *pRI) {
    ImsCdmaSms temp = {};

    if (message.cdmaMessage.size() != 1) {
        RLOGE("dispatchImsCdmaSms: Invalid len %s", requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return false;
    }

    temp.imsSms.tech = RADIO_TECH_3GPP2;
    temp.imsSms.retry = BOOL_TO_INT(message.retry);
    temp.imsSms.messageRef = message.messageRef;
    temp.imsSms.message.cdmaMessage = &temp.cdmaSms;

    constructCdmaSms(temp.cdmaSms, message.cdmaMessage[0]);

    // Vendor code expects payload length to include actual msg payload
    // (sizeof(RIL_CDMA_SMS_Message)) instead of (RIL_CDMA_SMS_Message *) + size of other fields in
    // RIL_IMS_SMS_Message
    int payloadLen = sizeof(RIL_RadioTechnologyFamily) + sizeof(uint8_t) + sizeof(int32_t)
            + sizeof(RIL_CDMA_SMS_Message);

    CALL_ONREQUEST(pRI->pCI->requestNumber, &temp.imsSms, payloadLen, pRI, pRI->socket_id);

    return true;
}

Return<void> RadioImpl::sendImsSms(int32_t serial, const ImsSmsMessage& message) {
#if VDBG
    RLOGD("sendImsSms: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_IMS_SEND_SMS);
    if (pRI == NULL) {
        return Void();
    }

    RIL_RadioTechnologyFamily format = (RIL_RadioTechnologyFamily) message.tech;

    if (RADIO_TECH_3GPP == format) {
        dispatchImsGsmSms(message, pRI);
    } else if (RADIO_TECH_3GPP2 == format) {
        dispatchImsCdmaSms(message, pRI);
    } else {
        RLOGE("sendImsSms: Invalid radio tech %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
    }
    return Void();
}

Return<void> RadioImpl::iccTransmitApduBasicChannel(int32_t serial, const SimApdu& message) {
#if VDBG
    RLOGD("iccTransmitApduBasicChannel: serial %d", serial);
#endif
    dispatchIccApdu(serial, mSlotId, RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC, message);
    return Void();
}

Return<void> RadioImpl::iccOpenLogicalChannel(int32_t serial, const hidl_string& aid, int32_t p2) {
#if VDBG
    RLOGD("iccOpenLogicalChannel: serial %d", serial);
#endif
    if (s_vendorFunctions->version < 15) {
        dispatchString(serial, mSlotId, RIL_REQUEST_SIM_OPEN_CHANNEL, aid.c_str());
    } else {
        RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SIM_OPEN_CHANNEL);
        if (pRI == NULL) {
            return Void();
        }

        RIL_OpenChannelParams params = {};

        params.p2 = p2;

        if (!copyHidlStringToRil(&params.aidPtr, aid, pRI)) {
            return Void();
        }

        CALL_ONREQUEST(pRI->pCI->requestNumber, &params, sizeof(params), pRI, mSlotId);

        memsetAndFreeStrings(1, params.aidPtr);
    }
    return Void();
}

Return<void> RadioImpl::iccCloseLogicalChannel(int32_t serial, int32_t channelId) {
#if VDBG
    RLOGD("iccCloseLogicalChannel: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SIM_CLOSE_CHANNEL, 1, channelId);
    return Void();
}

Return<void> RadioImpl::iccTransmitApduLogicalChannel(int32_t serial, const SimApdu& message) {
#if VDBG
    RLOGD("iccTransmitApduLogicalChannel: serial %d", serial);
#endif
    dispatchIccApdu(serial, mSlotId, RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL, message);
    return Void();
}

Return<void> RadioImpl::nvReadItem(int32_t serial, NvItem itemId) {
#if VDBG
    RLOGD("nvReadItem: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_NV_READ_ITEM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_NV_ReadItem nvri = {};
    nvri.itemID = (RIL_NV_Item) itemId;

    CALL_ONREQUEST(pRI->pCI->requestNumber, &nvri, sizeof(nvri), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::nvWriteItem(int32_t serial, const NvWriteItem& item) {
#if VDBG
    RLOGD("nvWriteItem: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_NV_WRITE_ITEM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_NV_WriteItem nvwi = {};

    nvwi.itemID = (RIL_NV_Item) item.itemId;

    if (!copyHidlStringToRil(&nvwi.value, item.value, pRI)) {
        return Void();
    }

    CALL_ONREQUEST(pRI->pCI->requestNumber, &nvwi, sizeof(nvwi), pRI, mSlotId);

    memsetAndFreeStrings(1, nvwi.value);
    return Void();
}

Return<void> RadioImpl::nvWriteCdmaPrl(int32_t serial, const hidl_vec<uint8_t>& prl) {
#if VDBG
    RLOGD("nvWriteCdmaPrl: serial %d", serial);
#endif
    dispatchRaw(serial, mSlotId, RIL_REQUEST_NV_WRITE_CDMA_PRL, prl);
    return Void();
}

Return<void> RadioImpl::nvResetConfig(int32_t serial, ResetNvType resetType) {
    int rilResetType = -1;
#if VDBG
    RLOGD("nvResetConfig: serial %d", serial);
#endif
    /* Convert ResetNvType to RIL.h values
     * RIL_REQUEST_NV_RESET_CONFIG
     * 1 - reload all NV items
     * 2 - erase NV reset (SCRTN)
     * 3 - factory reset (RTN)
     */
    switch(resetType) {
      case ResetNvType::RELOAD:
        rilResetType = 1;
        break;
      case ResetNvType::ERASE:
        rilResetType = 2;
        break;
      case ResetNvType::FACTORY_RESET:
        rilResetType = 3;
        break;
    }
    dispatchInts(serial, mSlotId, RIL_REQUEST_NV_RESET_CONFIG, 1, rilResetType);
    return Void();
}

Return<void> RadioImpl::setUiccSubscription(int32_t serial, const SelectUiccSub& uiccSub) {
#if VDBG
    RLOGD("setUiccSubscription: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_UICC_SUBSCRIPTION);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SelectUiccSub rilUiccSub = {};

    rilUiccSub.slot = uiccSub.slot;
    rilUiccSub.app_index = uiccSub.appIndex;
    rilUiccSub.sub_type = (RIL_SubscriptionType) uiccSub.subType;
    rilUiccSub.act_status = (RIL_UiccSubActStatus) uiccSub.actStatus;

    CALL_ONREQUEST(pRI->pCI->requestNumber, &rilUiccSub, sizeof(rilUiccSub), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::setDataAllowed(int32_t serial, bool allow) {
#if VDBG
    RLOGD("setDataAllowed: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_ALLOW_DATA, 1, BOOL_TO_INT(allow));
    return Void();
}

Return<void> RadioImpl::getHardwareConfig(int32_t serial) {
#if VDBG
    RLOGD("getHardwareConfig: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_HARDWARE_CONFIG);
    return Void();
}

Return<void> RadioImpl::requestIccSimAuthentication(int32_t serial, int32_t authContext,
        const hidl_string& authData, const hidl_string& aid) {
#if VDBG
    RLOGD("requestIccSimAuthentication: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SIM_AUTHENTICATION);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SimAuthentication pf = {};

    pf.authContext = authContext;

    if (!copyHidlStringToRil(&pf.authData, authData, pRI)) {
        return Void();
    }

    if (!copyHidlStringToRil(&pf.aid, aid, pRI)) {
        memsetAndFreeStrings(1, pf.authData);
        return Void();
    }

    CALL_ONREQUEST(pRI->pCI->requestNumber, &pf, sizeof(pf), pRI, mSlotId);

    memsetAndFreeStrings(2, pf.authData, pf.aid);
    return Void();
}

/**
 * @param numProfiles number of data profile
 * @param dataProfiles the pointer to the actual data profiles. The acceptable type is
          RIL_DataProfileInfo or RIL_DataProfileInfo_v15.
 * @param dataProfilePtrs the pointer to the pointers that point to each data profile structure
 * @param numfields number of string-type member in the data profile structure
 * @param ... the variadic parameters are pointers to each string-type member
 **/
template <typename T>
void freeSetDataProfileData(int numProfiles, T *dataProfiles, T **dataProfilePtrs,
                            int numfields, ...) {
    va_list args;
    va_start(args, numfields);

    // Iterate through each string-type field that need to be free.
    for (int i = 0; i < numfields; i++) {
        // Iterate through each data profile and free that specific string-type field.
        // The type 'char *T::*' is a type of pointer to a 'char *' member inside T structure.
        char *T::*ptr = va_arg(args, char *T::*);
        for (int j = 0; j < numProfiles; j++) {
            memsetAndFreeStrings(1, dataProfiles[j].*ptr);
        }
    }

    va_end(args);

#ifdef MEMSET_FREED
    memset(dataProfiles, 0, numProfiles * sizeof(T));
    memset(dataProfilePtrs, 0, numProfiles * sizeof(T *));
#endif
    free(dataProfiles);
    free(dataProfilePtrs);
}

Return<void> RadioImpl::setDataProfile(int32_t serial, const hidl_vec<DataProfileInfo>& profiles,
                                       bool isRoaming) {
#if VDBG
    RLOGD("setDataProfile: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SET_DATA_PROFILE);
    if (pRI == NULL) {
        return Void();
    }

    size_t num = profiles.size();
    bool success = false;

    if (s_vendorFunctions->version <= 14) {

        RIL_DataProfileInfo *dataProfiles =
            (RIL_DataProfileInfo *) calloc(num, sizeof(RIL_DataProfileInfo));

        if (dataProfiles == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        RIL_DataProfileInfo **dataProfilePtrs =
            (RIL_DataProfileInfo **) calloc(num, sizeof(RIL_DataProfileInfo *));
        if (dataProfilePtrs == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            free(dataProfiles);
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        for (size_t i = 0; i < num; i++) {
            dataProfilePtrs[i] = &dataProfiles[i];

            success = copyHidlStringToRil(&dataProfiles[i].apn, profiles[i].apn, pRI, true);

            const hidl_string &protocol =
                    (isRoaming ? profiles[i].roamingProtocol : profiles[i].protocol);

            if (success && !copyHidlStringToRil(&dataProfiles[i].protocol, protocol, pRI, true)) {
                success = false;
            }

            if (success && !copyHidlStringToRil(&dataProfiles[i].user, profiles[i].user, pRI,
                    true)) {
                success = false;
            }
            if (success && !copyHidlStringToRil(&dataProfiles[i].password, profiles[i].password,
                    pRI, true)) {
                success = false;
            }

            if (!success) {
                freeSetDataProfileData(num, dataProfiles, dataProfilePtrs, 4,
                    &RIL_DataProfileInfo::apn, &RIL_DataProfileInfo::protocol,
                    &RIL_DataProfileInfo::user, &RIL_DataProfileInfo::password);
                return Void();
            }

            dataProfiles[i].profileId = (RIL_DataProfile) profiles[i].profileId;
            dataProfiles[i].authType = (int) profiles[i].authType;
            dataProfiles[i].type = (int) profiles[i].type;
            dataProfiles[i].maxConnsTime = profiles[i].maxConnsTime;
            dataProfiles[i].maxConns = profiles[i].maxConns;
            dataProfiles[i].waitTime = profiles[i].waitTime;
            dataProfiles[i].enabled = BOOL_TO_INT(profiles[i].enabled);
        }

        CALL_ONREQUEST(RIL_REQUEST_SET_DATA_PROFILE, dataProfilePtrs,
                num * sizeof(RIL_DataProfileInfo *), pRI, mSlotId);

        freeSetDataProfileData(num, dataProfiles, dataProfilePtrs, 4,
                &RIL_DataProfileInfo::apn, &RIL_DataProfileInfo::protocol,
                &RIL_DataProfileInfo::user, &RIL_DataProfileInfo::password);
    } else {
        RIL_DataProfileInfo_v15 *dataProfiles =
            (RIL_DataProfileInfo_v15 *) calloc(num, sizeof(RIL_DataProfileInfo_v15));

        if (dataProfiles == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        RIL_DataProfileInfo_v15 **dataProfilePtrs =
            (RIL_DataProfileInfo_v15 **) calloc(num, sizeof(RIL_DataProfileInfo_v15 *));
        if (dataProfilePtrs == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            free(dataProfiles);
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        for (size_t i = 0; i < num; i++) {
            dataProfilePtrs[i] = &dataProfiles[i];

            success = copyHidlStringToRil(&dataProfiles[i].apn, profiles[i].apn, pRI, true);
            if (success && !copyHidlStringToRil(&dataProfiles[i].protocol, profiles[i].protocol,
                    pRI)) {
                success = false;
            }
            if (success && !copyHidlStringToRil(&dataProfiles[i].roamingProtocol,
                    profiles[i].roamingProtocol, pRI, true)) {
                success = false;
            }
            if (success && !copyHidlStringToRil(&dataProfiles[i].user, profiles[i].user, pRI,
                    true)) {
                success = false;
            }
            if (success && !copyHidlStringToRil(&dataProfiles[i].password, profiles[i].password,
                    pRI, true)) {
                success = false;
            }
            if (success && !copyHidlStringToRil(&dataProfiles[i].mvnoMatchData,
                    profiles[i].mvnoMatchData, pRI, true)) {
                success = false;
            }

            if (success && !convertMvnoTypeToString(profiles[i].mvnoType,
                    dataProfiles[i].mvnoType)) {
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                success = false;
            }

            if (!success) {
                freeSetDataProfileData(num, dataProfiles, dataProfilePtrs, 6,
                    &RIL_DataProfileInfo_v15::apn, &RIL_DataProfileInfo_v15::protocol,
                    &RIL_DataProfileInfo_v15::roamingProtocol, &RIL_DataProfileInfo_v15::user,
                    &RIL_DataProfileInfo_v15::password, &RIL_DataProfileInfo_v15::mvnoMatchData);
                return Void();
            }

            dataProfiles[i].profileId = (RIL_DataProfile) profiles[i].profileId;
            dataProfiles[i].authType = (int) profiles[i].authType;
            dataProfiles[i].type = (int) profiles[i].type;
            dataProfiles[i].maxConnsTime = profiles[i].maxConnsTime;
            dataProfiles[i].maxConns = profiles[i].maxConns;
            dataProfiles[i].waitTime = profiles[i].waitTime;
            dataProfiles[i].enabled = BOOL_TO_INT(profiles[i].enabled);
            dataProfiles[i].supportedTypesBitmask = profiles[i].supportedApnTypesBitmap;
            dataProfiles[i].bearerBitmask = profiles[i].bearerBitmap;
            dataProfiles[i].mtu = profiles[i].mtu;
        }

        CALL_ONREQUEST(RIL_REQUEST_SET_DATA_PROFILE, dataProfilePtrs,
                num * sizeof(RIL_DataProfileInfo_v15 *), pRI, mSlotId);

        freeSetDataProfileData(num, dataProfiles, dataProfilePtrs, 6,
                &RIL_DataProfileInfo_v15::apn, &RIL_DataProfileInfo_v15::protocol,
                &RIL_DataProfileInfo_v15::roamingProtocol, &RIL_DataProfileInfo_v15::user,
                &RIL_DataProfileInfo_v15::password, &RIL_DataProfileInfo_v15::mvnoMatchData);
    }

    return Void();
}

Return<void> RadioImpl::requestShutdown(int32_t serial) {
#if VDBG
    RLOGD("requestShutdown: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SHUTDOWN);
    return Void();
}

Return<void> RadioImpl::getRadioCapability(int32_t serial) {
#if VDBG
    RLOGD("getRadioCapability: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_RADIO_CAPABILITY);
    return Void();
}

Return<void> RadioImpl::setRadioCapability(int32_t serial, const RadioCapability& rc) {
#if VDBG
    RLOGD("setRadioCapability: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SET_RADIO_CAPABILITY);
    if (pRI == NULL) {
        return Void();
    }

    RIL_RadioCapability rilRc = {};

    // TODO : set rilRc.version using HIDL version ?
    rilRc.session = rc.session;
    rilRc.phase = (int) rc.phase;
    rilRc.rat = (int) rc.raf;
    rilRc.status = (int) rc.status;
    strlcpy(rilRc.logicalModemUuid, rc.logicalModemUuid.c_str(), sizeof(rilRc.logicalModemUuid));

    CALL_ONREQUEST(pRI->pCI->requestNumber, &rilRc, sizeof(rilRc), pRI, mSlotId);

    return Void();
}

Return<void> RadioImpl::startLceService(int32_t serial, int32_t reportInterval, bool pullMode) {
#if VDBG
    RLOGD("startLceService: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_START_LCE, 2, reportInterval,
            BOOL_TO_INT(pullMode));
    return Void();
}

Return<void> RadioImpl::stopLceService(int32_t serial) {
#if VDBG
    RLOGD("stopLceService: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_STOP_LCE);
    return Void();
}

Return<void> RadioImpl::pullLceData(int32_t serial) {
#if VDBG
    RLOGD("pullLceData: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_PULL_LCEDATA);
    return Void();
}

Return<void> RadioImpl::getModemActivityInfo(int32_t serial) {
#if VDBG
    RLOGD("getModemActivityInfo: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_ACTIVITY_INFO);
    return Void();
}

Return<void> RadioImpl::setAllowedCarriers(int32_t serial, bool allAllowed,
                                           const CarrierRestrictions& carriers) {
#if VDBG
    RLOGD("setAllowedCarriers: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_CARRIER_RESTRICTIONS);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CarrierRestrictions cr = {};
    RIL_Carrier *allowedCarriers = NULL;
    RIL_Carrier *excludedCarriers = NULL;

    cr.len_allowed_carriers = carriers.allowedCarriers.size();
    allowedCarriers = (RIL_Carrier *)calloc(cr.len_allowed_carriers, sizeof(RIL_Carrier));
    if (allowedCarriers == NULL) {
        RLOGE("setAllowedCarriers: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    cr.allowed_carriers = allowedCarriers;

    cr.len_excluded_carriers = carriers.excludedCarriers.size();
    excludedCarriers = (RIL_Carrier *)calloc(cr.len_excluded_carriers, sizeof(RIL_Carrier));
    if (excludedCarriers == NULL) {
        RLOGE("setAllowedCarriers: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
#ifdef MEMSET_FREED
        memset(allowedCarriers, 0, cr.len_allowed_carriers * sizeof(RIL_Carrier));
#endif
        free(allowedCarriers);
        return Void();
    }
    cr.excluded_carriers = excludedCarriers;

    for (int i = 0; i < cr.len_allowed_carriers; i++) {
        allowedCarriers[i].mcc = carriers.allowedCarriers[i].mcc.c_str();
        allowedCarriers[i].mnc = carriers.allowedCarriers[i].mnc.c_str();
        allowedCarriers[i].match_type = (RIL_CarrierMatchType) carriers.allowedCarriers[i].matchType;
        allowedCarriers[i].match_data = carriers.allowedCarriers[i].matchData.c_str();
    }

    for (int i = 0; i < cr.len_excluded_carriers; i++) {
        excludedCarriers[i].mcc = carriers.excludedCarriers[i].mcc.c_str();
        excludedCarriers[i].mnc = carriers.excludedCarriers[i].mnc.c_str();
        excludedCarriers[i].match_type =
                (RIL_CarrierMatchType) carriers.excludedCarriers[i].matchType;
        excludedCarriers[i].match_data = carriers.excludedCarriers[i].matchData.c_str();
    }

    CALL_ONREQUEST(pRI->pCI->requestNumber, &cr, sizeof(RIL_CarrierRestrictions), pRI, mSlotId);

#ifdef MEMSET_FREED
    memset(allowedCarriers, 0, cr.len_allowed_carriers * sizeof(RIL_Carrier));
    memset(excludedCarriers, 0, cr.len_excluded_carriers * sizeof(RIL_Carrier));
#endif
    free(allowedCarriers);
    free(excludedCarriers);
    return Void();
}

Return<void> RadioImpl::getAllowedCarriers(int32_t serial) {
#if VDBG
    RLOGD("getAllowedCarriers: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CARRIER_RESTRICTIONS);
    return Void();
}

Return<void> RadioImpl::sendDeviceState(int32_t serial, DeviceStateType deviceStateType,
                                        bool state) {
#if VDBG
    RLOGD("sendDeviceState: serial %d", serial);
#endif
    if (s_vendorFunctions->version < 15) {
        if (deviceStateType ==  DeviceStateType::LOW_DATA_EXPECTED) {
            RLOGD("sendDeviceState: calling screen state %d", BOOL_TO_INT(!state));
            dispatchInts(serial, mSlotId, RIL_REQUEST_SCREEN_STATE, 1, BOOL_TO_INT(!state));
        } else {
            RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                    RIL_REQUEST_SEND_DEVICE_STATE);
            sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        }
        return Void();
    }
    dispatchInts(serial, mSlotId, RIL_REQUEST_SEND_DEVICE_STATE, 2, (int) deviceStateType,
            BOOL_TO_INT(state));
    return Void();
}

Return<void> RadioImpl::setIndicationFilter(int32_t serial, int32_t indicationFilter) {
#if VDBG
    RLOGD("setIndicationFilter: serial %d", serial);
#endif
    if (s_vendorFunctions->version < 15) {
        RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER);
        sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        return Void();
    }
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER, 1, indicationFilter);
    return Void();
}

Return<void> RadioImpl::setSimCardPower(int32_t serial, bool powerUp) {
#if VDBG
    RLOGD("setSimCardPower: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_SIM_CARD_POWER, 1, BOOL_TO_INT(powerUp));
    return Void();
}

Return<void> RadioImpl::setSimCardPower_1_1(int32_t serial, const V1_1::CardPowerState state) {
#if VDBG
    RLOGD("setSimCardPower_1_1: serial %d state %d", serial, state);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_SIM_CARD_POWER, 1, state);
    return Void();
}

Return<void> RadioImpl::setCarrierInfoForImsiEncryption(int32_t serial,
        const V1_1::ImsiEncryptionInfo& data) {
#if VDBG
    RLOGD("setCarrierInfoForImsiEncryption: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(
            serial, mSlotId, RIL_REQUEST_SET_CARRIER_INFO_IMSI_ENCRYPTION);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CarrierInfoForImsiEncryption imsiEncryption = {};

    if (!copyHidlStringToRil(&imsiEncryption.mnc, data.mnc, pRI)) {
        return Void();
    }
    if (!copyHidlStringToRil(&imsiEncryption.mcc, data.mcc, pRI)) {
        memsetAndFreeStrings(1, imsiEncryption.mnc);
        return Void();
    }
    if (!copyHidlStringToRil(&imsiEncryption.keyIdentifier, data.keyIdentifier, pRI)) {
        memsetAndFreeStrings(2, imsiEncryption.mnc, imsiEncryption.mcc);
        return Void();
    }
    imsiEncryption.carrierKeyLength = data.carrierKey.size();
    imsiEncryption.carrierKey = new uint8_t[imsiEncryption.carrierKeyLength];
    memcpy(imsiEncryption.carrierKey, data.carrierKey.data(), imsiEncryption.carrierKeyLength);
    imsiEncryption.expirationTime = data.expirationTime;
    CALL_ONREQUEST(pRI->pCI->requestNumber, &imsiEncryption,
            sizeof(RIL_CarrierInfoForImsiEncryption), pRI, mSlotId);
    delete(imsiEncryption.carrierKey);
    return Void();
}

Return<void> RadioImpl::startKeepalive(int32_t serial, const V1_1::KeepaliveRequest& keepalive) {
#if VDBG
    RLOGD("%s(): %d", __FUNCTION__, serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_START_KEEPALIVE);
    if (pRI == NULL) {
        return Void();
    }

    RIL_KeepaliveRequest kaReq = {};

    kaReq.type = static_cast<RIL_KeepaliveType>(keepalive.type);
    switch(kaReq.type) {
        case NATT_IPV4:
            if (keepalive.sourceAddress.size() != 4 ||
                    keepalive.destinationAddress.size() != 4) {
                RLOGE("Invalid address for keepalive!");
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                return Void();
            }
            break;
        case NATT_IPV6:
            if (keepalive.sourceAddress.size() != 16 ||
                    keepalive.destinationAddress.size() != 16) {
                RLOGE("Invalid address for keepalive!");
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                return Void();
            }
            break;
        default:
            RLOGE("Unknown packet keepalive type!");
            sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            return Void();
    }

    ::memcpy(kaReq.sourceAddress, keepalive.sourceAddress.data(), keepalive.sourceAddress.size());
    kaReq.sourcePort = keepalive.sourcePort;

    ::memcpy(kaReq.destinationAddress,
            keepalive.destinationAddress.data(), keepalive.destinationAddress.size());
    kaReq.destinationPort = keepalive.destinationPort;

    kaReq.maxKeepaliveIntervalMillis = keepalive.maxKeepaliveIntervalMillis;
    kaReq.cid = keepalive.cid; // This is the context ID of the data call

    CALL_ONREQUEST(pRI->pCI->requestNumber, &kaReq, sizeof(RIL_KeepaliveRequest), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::stopKeepalive(int32_t serial, int32_t sessionHandle) {
#if VDBG
    RLOGD("%s(): %d", __FUNCTION__, serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_STOP_KEEPALIVE);
    if (pRI == NULL) {
        return Void();
    }

    CALL_ONREQUEST(pRI->pCI->requestNumber, &sessionHandle, sizeof(uint32_t), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::responseAcknowledgement() {
    android::releaseWakeLock();
    return Void();
}

Return<void> OemHookImpl::setResponseFunctions(
        const ::android::sp<IOemHookResponse>& oemHookResponseParam,
        const ::android::sp<IOemHookIndication>& oemHookIndicationParam) {
#if VDBG
    RLOGD("OemHookImpl::setResponseFunctions");
#endif

    pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(mSlotId);
    int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
    assert(ret == 0);

    mOemHookResponse = oemHookResponseParam;
    mOemHookIndication = oemHookIndicationParam;
    mCounterOemHook[mSlotId]++;

    ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
    assert(ret == 0);

    return Void();
}

Return<void> OemHookImpl::sendRequestRaw(int32_t serial, const hidl_vec<uint8_t>& data) {
#if VDBG
    RLOGD("OemHookImpl::sendRequestRaw: serial %d", serial);
#endif
    dispatchRaw(serial, mSlotId, RIL_REQUEST_OEM_HOOK_RAW, data);
    return Void();
}

Return<void> OemHookImpl::sendRequestStrings(int32_t serial,
        const hidl_vec<hidl_string>& data) {
#if VDBG
    RLOGD("OemHookImpl::sendRequestStrings: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_OEM_HOOK_STRINGS, data);
    return Void();
}

/***************************************************************************************************
 * RESPONSE FUNCTIONS
 * Functions above are used for requests going from framework to vendor code. The ones below are
 * responses for those requests coming back from the vendor code.
 **************************************************************************************************/

void radio::acknowledgeRequest(int slotId, int serial) {
    if (radioService[slotId]->mRadioResponse != NULL) {
        Return<void> retStatus = radioService[slotId]->mRadioResponse->acknowledgeRequest(serial);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeRequest: radioService[%d]->mRadioResponse == NULL", slotId);
    }
}

void populateResponseInfo(RadioResponseInfo& responseInfo, int serial, int responseType,
                         RIL_Errno e) {
    responseInfo.serial = serial;
    switch (responseType) {
        case RESPONSE_SOLICITED:
            responseInfo.type = RadioResponseType::SOLICITED;
            break;
        case RESPONSE_SOLICITED_ACK_EXP:
            responseInfo.type = RadioResponseType::SOLICITED_ACK_EXP;
            break;
    }
    responseInfo.error = (RadioError) e;
}

static void populateResponseInfo_1_6(V1_6::RadioResponseInfo& responseInfo, int serial,
        int responseType, RIL_Errno e) {
    responseInfo.serial = serial;
    switch (responseType) {
        case RESPONSE_SOLICITED:
            responseInfo.type = RadioResponseType::SOLICITED;
            break;
        case RESPONSE_SOLICITED_ACK_EXP:
            responseInfo.type = RadioResponseType::SOLICITED_ACK_EXP;
            break;
        default:
            responseInfo.type = RadioResponseType::SOLICITED;
            break;
    }
    responseInfo.error = static_cast<V1_6::RadioError>(e);
}

static bool splitOperatorNumeric(const std::string& numeric, hidl_string& mcc,
        hidl_string& mnc) {
    if (!isValidOperatorNumeric(numeric.c_str())) return false;
    mcc = numeric.substr(0, 3);
    mnc = numeric.substr(3);
    return true;
}

static void fillMissingPlmn(hidl_string& mcc, hidl_string& mnc,
        const std::string& operatorNumeric) {
    if ((mcc.size() != 0 && mnc.size() != 0) || operatorNumeric.empty()) return;

    hidl_string fallbackMcc;
    hidl_string fallbackMnc;
    if (!splitOperatorNumeric(operatorNumeric, fallbackMcc, fallbackMnc)) return;

    if (mcc.size() == 0) mcc = fallbackMcc;
    if (mnc.size() == 0) mnc = fallbackMnc;
}

static void convertCellIdentityToHal_1_5(const CellIdentity& in, V1_5::CellIdentity& out,
        const std::string& operatorNumeric = std::string()) {
    switch (in.cellInfoType) {
        case CellInfoType::GSM:
            if (in.cellIdentityGsm.size() != 0) {
                V1_5::CellIdentityGsm gsm = {};
                gsm.base.base = in.cellIdentityGsm[0];
                fillMissingPlmn(gsm.base.base.mcc, gsm.base.base.mnc, operatorNumeric);
                out.gsm(gsm);
            }
            break;
        case CellInfoType::WCDMA:
            if (in.cellIdentityWcdma.size() != 0) {
                V1_5::CellIdentityWcdma wcdma = {};
                wcdma.base.base = in.cellIdentityWcdma[0];
                fillMissingPlmn(wcdma.base.base.mcc, wcdma.base.base.mnc, operatorNumeric);
                out.wcdma(wcdma);
            }
            break;
        case CellInfoType::CDMA:
            if (in.cellIdentityCdma.size() != 0) {
                V1_2::CellIdentityCdma cdma = {};
                cdma.base = in.cellIdentityCdma[0];
                out.cdma(cdma);
            }
            break;
        case CellInfoType::LTE:
            if (in.cellIdentityLte.size() != 0) {
                V1_5::CellIdentityLte lte = {};
                lte.base.base = in.cellIdentityLte[0];
                fillMissingPlmn(lte.base.base.mcc, lte.base.base.mnc, operatorNumeric);
                out.lte(lte);
            }
            break;
        case CellInfoType::TD_SCDMA:
            if (in.cellIdentityTdscdma.size() != 0) {
                V1_5::CellIdentityTdscdma tdscdma = {};
                tdscdma.base.base = in.cellIdentityTdscdma[0];
                fillMissingPlmn(tdscdma.base.base.mcc, tdscdma.base.base.mnc, operatorNumeric);
                out.tdscdma(tdscdma);
            }
            break;
        default:
            break;
    }
}

static hidl_string registeredPlmnFromCellIdentity(const CellIdentity& cellIdentity) {
    hidl_string mcc;
    hidl_string mnc;
    switch (cellIdentity.cellInfoType) {
        case CellInfoType::GSM:
            if (cellIdentity.cellIdentityGsm.size() != 0) {
                mcc = cellIdentity.cellIdentityGsm[0].mcc;
                mnc = cellIdentity.cellIdentityGsm[0].mnc;
            }
            break;
        case CellInfoType::WCDMA:
            if (cellIdentity.cellIdentityWcdma.size() != 0) {
                mcc = cellIdentity.cellIdentityWcdma[0].mcc;
                mnc = cellIdentity.cellIdentityWcdma[0].mnc;
            }
            break;
        case CellInfoType::LTE:
            if (cellIdentity.cellIdentityLte.size() != 0) {
                mcc = cellIdentity.cellIdentityLte[0].mcc;
                mnc = cellIdentity.cellIdentityLte[0].mnc;
            }
            break;
        case CellInfoType::TD_SCDMA:
            if (cellIdentity.cellIdentityTdscdma.size() != 0) {
                mcc = cellIdentity.cellIdentityTdscdma[0].mcc;
                mnc = cellIdentity.cellIdentityTdscdma[0].mnc;
            }
            break;
        default:
            break;
    }
    if (mcc.empty() || mnc.empty()) return {};
    return hidl_string(std::string(mcc.c_str()) + mnc.c_str());
}

static hidl_string registeredPlmnForResult(int slotId, RegState regState,
        const CellIdentity& cellIdentity) {
    hidl_string registeredPlmn = registeredPlmnFromCellIdentity(cellIdentity);
    if (!registeredPlmn.empty()) return registeredPlmn;

    // Old Samsung registration replies have LAC/CID but no MCC/MNC. HIDL 1.6
    // added registeredPlmn, so join it with the separately returned OPERATOR
    // value only while the modem itself reports HOME or ROAMING.
    if (regState != RegState::REG_HOME && regState != RegState::REG_ROAMING) return {};
    const std::string cached = getCachedOperatorNumeric(slotId);
    return cached.empty() ? hidl_string() : hidl_string(cached);
}

static void convertVoiceRegStateToHal_1_6(int slotId, const VoiceRegStateResult& in,
        V1_6::RegStateResult& out) {
    out.regState = in.regState;
    out.rat = static_cast<V1_4::RadioTechnology>(in.rat);
    out.reasonForDenial = static_cast<V1_5::RegistrationFailCause>(in.reasonForDenial);
    const bool registered = in.regState == RegState::REG_HOME ||
            in.regState == RegState::REG_ROAMING;
    const std::string operatorNumeric = registered ? getCachedOperatorNumeric(slotId)
                                                   : std::string();
    convertCellIdentityToHal_1_5(in.cellIdentity, out.cellIdentity, operatorNumeric);
    out.registeredPlmn = registeredPlmnForResult(slotId, in.regState, in.cellIdentity);

    switch (in.rat) {
        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD: {
            V1_5::RegStateResult::AccessTechnologySpecificInfo::Cdma2000RegistrationInfo cdma = {};
            cdma.cssSupported = in.cssSupported;
            cdma.roamingIndicator = in.roamingIndicator;
            cdma.systemIsInPrl = static_cast<V1_5::PrlIndicator>(in.systemIsInPrl);
            cdma.defaultRoamingIndicator = in.defaultRoamingIndicator;
            out.accessTechnologySpecificInfo.cdmaInfo(cdma);
            break;
        }
        case RADIO_TECH_LTE:
        case RADIO_TECH_LTE_CA: {
            V1_5::RegStateResult::AccessTechnologySpecificInfo::EutranRegistrationInfo eutran = {};
            out.accessTechnologySpecificInfo.eutranInfo(eutran);
            break;
        }
        case RADIO_TECH_GPRS:
        case RADIO_TECH_EDGE:
        case RADIO_TECH_GSM:
            out.accessTechnologySpecificInfo.geranDtmSupported(false);
            break;
        default:
            break;
    }
}

static void convertDataRegStateToHal_1_6(int slotId, const DataRegStateResult& in,
        V1_6::RegStateResult& out) {
    out.regState = in.regState;
    out.rat = static_cast<V1_4::RadioTechnology>(in.rat);
    out.reasonForDenial = static_cast<V1_5::RegistrationFailCause>(in.reasonDataDenied);
    const bool registered = in.regState == RegState::REG_HOME ||
            in.regState == RegState::REG_ROAMING;
    const std::string operatorNumeric = registered ? getCachedOperatorNumeric(slotId)
                                                   : std::string();
    convertCellIdentityToHal_1_5(in.cellIdentity, out.cellIdentity, operatorNumeric);
    out.registeredPlmn = registeredPlmnForResult(slotId, in.regState, in.cellIdentity);

    if (in.rat == RADIO_TECH_LTE || in.rat == RADIO_TECH_LTE_CA) {
        V1_5::RegStateResult::AccessTechnologySpecificInfo::EutranRegistrationInfo eutran = {};
        out.accessTechnologySpecificInfo.eutranInfo(eutran);
    } else if (in.rat == RADIO_TECH_GPRS || in.rat == RADIO_TECH_EDGE ||
            in.rat == RADIO_TECH_GSM) {
        out.accessTechnologySpecificInfo.geranDtmSupported(false);
    }
}

int responseIntOrEmpty(RadioResponseInfo& responseInfo, int serial, int responseType, RIL_Errno e,
               void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    int ret = -1;

    if (response == NULL && responseLen == 0) {
        // Earlier RILs did not send a response for some cases although the interface
        // expected an integer as response. Do not return error if response is empty. Instead
        // Return -1 in those cases to maintain backward compatibility.
    } else if (response == NULL || responseLen % sizeof(int) != 0) {
        RLOGE("responseIntOrEmpty: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        int *p_int = (int *) response;
        ret = p_int[0];
    }
    return ret;
}

int responseInt(RadioResponseInfo& responseInfo, int serial, int responseType, RIL_Errno e,
               void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    int ret = -1;

    if (response == NULL || responseLen % sizeof(int) != 0) {
        RLOGE("responseInt: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        int *p_int = (int *) response;
        ret = p_int[0];
    }
    return ret;
}

int radio::getIccCardStatusResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        CardStatus cardStatus = {
                CardState::ABSENT, PinState::UNKNOWN, -1, -1, -1, {}};
        const RIL_AppStatus *rilAppStatus = nullptr;
        int numApplications = 0;
        if (response == NULL) {
            RLOGE("getIccCardStatusResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else if (responseLen == sizeof(RIL_CardStatus_v5)) {
            const RIL_CardStatus_v5 *p_cur =
                    static_cast<const RIL_CardStatus_v5 *>(response);
            numApplications = p_cur->num_applications;
            if (numApplications < 0 || numApplications > RIL_CARD_MAX_APPS ||
                    p_cur->gsm_umts_subscription_app_index < -1 ||
                    p_cur->gsm_umts_subscription_app_index >= numApplications ||
                    p_cur->cdma_subscription_app_index < -1 ||
                    p_cur->cdma_subscription_app_index >= numApplications) {
                RLOGE("getIccCardStatusResponse: Invalid v5 application metadata");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                cardStatus.cardState = static_cast<CardState>(p_cur->card_state);
                cardStatus.universalPinState =
                        static_cast<PinState>(p_cur->universal_pin_state);
                cardStatus.gsmUmtsSubscriptionAppIndex =
                        p_cur->gsm_umts_subscription_app_index;
                cardStatus.cdmaSubscriptionAppIndex =
                        p_cur->cdma_subscription_app_index;
                cardStatus.imsSubscriptionAppIndex = -1;
                rilAppStatus = p_cur->applications;
            }
        } else if (responseLen == sizeof(RIL_CardStatus_v6)) {
            const RIL_CardStatus_v6 *p_cur =
                    static_cast<const RIL_CardStatus_v6 *>(response);
            numApplications = p_cur->num_applications;
            if (numApplications < 0 || numApplications > RIL_CARD_MAX_APPS ||
                    p_cur->gsm_umts_subscription_app_index < -1 ||
                    p_cur->gsm_umts_subscription_app_index >= numApplications ||
                    p_cur->cdma_subscription_app_index < -1 ||
                    p_cur->cdma_subscription_app_index >= numApplications ||
                    p_cur->ims_subscription_app_index < -1 ||
                    p_cur->ims_subscription_app_index >= numApplications) {
                RLOGE("getIccCardStatusResponse: Invalid v6 application metadata");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                cardStatus.cardState = static_cast<CardState>(p_cur->card_state);
                cardStatus.universalPinState =
                        static_cast<PinState>(p_cur->universal_pin_state);
                cardStatus.gsmUmtsSubscriptionAppIndex =
                        p_cur->gsm_umts_subscription_app_index;
                cardStatus.cdmaSubscriptionAppIndex =
                        p_cur->cdma_subscription_app_index;
                cardStatus.imsSubscriptionAppIndex =
                        p_cur->ims_subscription_app_index;
                rilAppStatus = p_cur->applications;
            }
        } else {
            RLOGE("%s: Invalid response: Unsupported RIL_CardStatus (%d)",
                __func__, responseLen);
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        }

        if (rilAppStatus != nullptr) {
            cardStatus.applications.resize(numApplications);
            AppStatus *appStatus = cardStatus.applications.data();
#if VDBG
            RLOGD("getIccCardStatusResponse: num_applications %d", numApplications);
#endif
            for (int i = 0; i < numApplications; i++) {
                appStatus[i].appType = static_cast<AppType>(rilAppStatus[i].app_type);
                appStatus[i].appState = static_cast<AppState>(rilAppStatus[i].app_state);
                appStatus[i].persoSubstate =
                        static_cast<PersoSubstate>(rilAppStatus[i].perso_substate);
                appStatus[i].aidPtr = convertCharPtrToHidlString(rilAppStatus[i].aid_ptr);
                appStatus[i].appLabelPtr =
                        convertCharPtrToHidlString(rilAppStatus[i].app_label_ptr);
                appStatus[i].pin1Replaced = rilAppStatus[i].pin1_replaced;
                appStatus[i].pin1 = static_cast<PinState>(rilAppStatus[i].pin1);
                appStatus[i].pin2 = static_cast<PinState>(rilAppStatus[i].pin2);
            }
        }

        if (cardStatus.cardState == CardState::ABSENT) {
            std::lock_guard<std::mutex> lock(sIccidCacheMutex);
            sCachedIccids.erase(slotId);
        }

        Return<void> retStatus;
        const std::string cachedIccid = getCachedIccid(slotId);
        if (radioService[slotId]->mRadioResponseV1_5 != nullptr) {
            V1_5::CardStatus cardStatusV1_5 = {};
            cardStatusV1_5.base.base.base = cardStatus;
            cardStatusV1_5.base.base.physicalSlotId = slotId;
            cardStatusV1_5.base.base.iccid = cachedIccid;
            // A legacy RIL_CardStatus has no ATR or eUICC EID fields. ICCID is
            // recovered from the modem's real EF_ICCID SIM_IO response.
            cardStatusV1_5.applications.resize(cardStatus.applications.size());
            for (size_t i = 0; i < cardStatus.applications.size(); i++) {
                cardStatusV1_5.applications[i].base = cardStatus.applications[i];
                cardStatusV1_5.applications[i].persoSubstate =
                        static_cast<V1_5::PersoSubstate>(
                                cardStatus.applications[i].persoSubstate);
            }
            RLOGD("getIccCardStatusResponse: replying through HIDL 1.5 callback");
            retStatus = radioService[slotId]->mRadioResponseV1_5->
                    getIccCardStatusResponse_1_5(responseInfo, cardStatusV1_5);
        } else if (radioService[slotId]->mRadioResponseV1_4 != nullptr) {
            V1_4::CardStatus cardStatusV1_4 = {};
            cardStatusV1_4.base.base = cardStatus;
            cardStatusV1_4.base.physicalSlotId = slotId;
            cardStatusV1_4.base.iccid = cachedIccid;
            RLOGD("getIccCardStatusResponse: replying through HIDL 1.4 callback");
            retStatus = radioService[slotId]->mRadioResponseV1_4->
                    getIccCardStatusResponse_1_4(responseInfo, cardStatusV1_4);
        } else if (radioService[slotId]->mRadioResponseV1_2 != nullptr) {
            V1_2::CardStatus cardStatusV1_2 = {};
            cardStatusV1_2.base = cardStatus;
            cardStatusV1_2.physicalSlotId = slotId;
            cardStatusV1_2.iccid = cachedIccid;
            RLOGD("getIccCardStatusResponse: replying through HIDL 1.2 callback");
            retStatus = radioService[slotId]->mRadioResponseV1_2->
                    getIccCardStatusResponse_1_2(responseInfo, cardStatusV1_2);
        } else {
            RLOGD("getIccCardStatusResponse: replying through HIDL 1.0 callback");
            retStatus = radioService[slotId]->mRadioResponse->
                    getIccCardStatusResponse(responseInfo, cardStatus);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getIccCardStatusResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::supplyIccPinForAppResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPinForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyIccPinForAppResponse(responseInfo, ret);
        RLOGE("supplyIccPinForAppResponse: amit ret %d", ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPinForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyIccPukForAppResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPukForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->supplyIccPukForAppResponse(
                responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPukForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyIccPin2ForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPin2ForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyIccPin2ForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPin2ForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyIccPuk2ForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPuk2ForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyIccPuk2ForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPuk2ForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::changeIccPinForAppResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("changeIccPinForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                changeIccPinForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("changeIccPinForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::changeIccPin2ForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("changeIccPin2ForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                changeIccPin2ForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("changeIccPin2ForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyNetworkDepersonalizationResponse(int slotId,
                                                 int responseType, int serial, RIL_Errno e,
                                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyNetworkDepersonalizationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyNetworkDepersonalizationResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyNetworkDepersonalizationResponse: radioService[%d]->mRadioResponse == "
                "NULL", slotId);
    }

    return 0;
}

int radio::supplySimDepersonalizationResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId]->mRadioResponseV1_5 != NULL) {
        RadioResponseInfo info = {};
        int retries = responseIntOrEmpty(
                info, serial, responseType, e, response, responseLen);
        Return<void> ret = radioService[slotId]->mRadioResponseV1_5
                ->supplySimDepersonalizationResponse(
                        info, V1_5::PersoSubstate::SIM_NETWORK, retries);
        radioService[slotId]->checkReturnStatus(ret);
    }
    return 0;
}

int radio::getCurrentCallsResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCurrentCallsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        bool valid = !((response == NULL && responseLen != 0) ||
                (responseLen % sizeof(RIL_Call *)) != 0);
        int num = valid ? responseLen / sizeof(RIL_Call *) : 0;
        if (valid && response != NULL) {
            for (int i = 0; i < num; ++i) {
                if (((RIL_Call **)response)[i] == NULL) {
                    valid = false;
                    num = 0;
                    break;
                }
            }
        }

        if ((response == NULL && responseLen != 0)
                || (responseLen % sizeof(RIL_Call *)) != 0) {
            RLOGE("getCurrentCallsResponse: Invalid response");
        }

        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo = {};
            populateResponseInfo_1_6(responseInfo, serial, responseType, e);
            if (!valid && e == RIL_E_SUCCESS) {
                responseInfo.error = V1_6::RadioError::INVALID_RESPONSE;
            }

            hidl_vec<V1_6::Call> calls;
            calls.resize(num);
            for (int i = 0; i < num; i++) {
                RIL_Call *p_cur = ((RIL_Call **) response)[i];
                Call& call = calls[i].base.base;
                call.state = static_cast<CallState>(p_cur->state);
                call.index = p_cur->index & 0xff;
                call.toa = p_cur->toa;
                call.isMpty = p_cur->isMpty;
                call.isMT = p_cur->isMT;
                call.als = p_cur->als;
                call.isVoice = p_cur->isVoice;
                call.isVoicePrivacy = p_cur->isVoicePrivacy;
                call.number = convertCharPtrToHidlString(p_cur->number);
                call.numberPresentation = static_cast<CallPresentation>(p_cur->numberPresentation);
                call.name = convertCharPtrToHidlString(p_cur->name);
                call.namePresentation = static_cast<CallPresentation>(p_cur->namePresentation);
                calls[i].base.audioQuality = V1_2::AudioQuality::UNSPECIFIED;
                calls[i].forwardedNumber = {};
                if (p_cur->uusInfo != NULL && p_cur->uusInfo->uusData != NULL) {
                    RIL_UUS_Info *uusInfo = p_cur->uusInfo;
                    call.uusInfo.resize(1);
                    call.uusInfo[0].uusType = static_cast<UusType>(uusInfo->uusType);
                    call.uusInfo[0].uusDcs = static_cast<UusDcs>(uusInfo->uusDcs);
                    char *nullTermStr = strndup(uusInfo->uusData, uusInfo->uusLength);
                    call.uusInfo[0].uusData = nullTermStr;
                    free(nullTermStr);
                }
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_6->
                    getCurrentCallsResponse_1_6(responseInfo, calls);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            if (!valid && e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            hidl_vec<Call> calls;
            calls.resize(num);
            for (int i = 0 ; i < num ; i++) {
                RIL_Call *p_cur = ((RIL_Call **) response)[i];
                /* each call info */
                calls[i].state = (CallState) p_cur->state;
                calls[i].index = p_cur->index & 0xff;
                calls[i].toa = p_cur->toa;
                calls[i].isMpty = p_cur->isMpty;
                calls[i].isMT = p_cur->isMT;
                calls[i].als = p_cur->als;
                calls[i].isVoice = p_cur->isVoice;
                calls[i].isVoicePrivacy = p_cur->isVoicePrivacy;
                calls[i].number = convertCharPtrToHidlString(p_cur->number);
                calls[i].numberPresentation = (CallPresentation) p_cur->numberPresentation;
                calls[i].name = convertCharPtrToHidlString(p_cur->name);
                calls[i].namePresentation = (CallPresentation) p_cur->namePresentation;
                if (p_cur->uusInfo != NULL && p_cur->uusInfo->uusData != NULL) {
                    RIL_UUS_Info *uusInfo = p_cur->uusInfo;
                    calls[i].uusInfo.resize(1);
                    calls[i].uusInfo[0].uusType = (UusType) uusInfo->uusType;
                    calls[i].uusInfo[0].uusDcs = (UusDcs) uusInfo->uusDcs;
                    // convert uusInfo->uusData to a null-terminated string
                    char *nullTermStr = strndup(uusInfo->uusData, uusInfo->uusLength);
                    calls[i].uusInfo[0].uusData = nullTermStr;
                    free(nullTermStr);
                }
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponse->
                    getCurrentCallsResponse(responseInfo, calls);
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getCurrentCallsResponse: no radio response callback for slot %d", slotId);
    }

    return 0;
}

int radio::dialResponse(int slotId,
                       int responseType, int serial, RIL_Errno e, void *response,
                       size_t responseLen) {
#if VDBG
    RLOGD("dialResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->dialResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("dialResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::emergencyDialResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *, size_t) {
    if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo info = {};
        populateResponseInfo(info, serial, responseType, e);
        Return<void> ret = radioService[slotId]->mRadioResponseV1_4
                ->emergencyDialResponse(info);
        radioService[slotId]->checkReturnStatus(ret);
    }
    return 0;
}

int radio::getIMSIForAppResponse(int slotId,
                                int responseType, int serial, RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
    RLOGD("getIMSIForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getIMSIForAppResponse(
                responseInfo, convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getIMSIForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::hangupConnectionResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("hangupConnectionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->hangupConnectionResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hangupConnectionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::hangupWaitingOrBackgroundResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("hangupWaitingOrBackgroundResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->hangupWaitingOrBackgroundResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hangupWaitingOrBackgroundResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::hangupForegroundResumeBackgroundResponse(int slotId, int responseType, int serial,
                                                    RIL_Errno e, void *response,
                                                    size_t responseLen) {
#if VDBG
    RLOGD("hangupWaitingOrBackgroundResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->hangupWaitingOrBackgroundResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hangupWaitingOrBackgroundResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::switchWaitingOrHoldingAndActiveResponse(int slotId, int responseType, int serial,
                                                   RIL_Errno e, void *response,
                                                   size_t responseLen) {
#if VDBG
    RLOGD("switchWaitingOrHoldingAndActiveResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->switchWaitingOrHoldingAndActiveResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("switchWaitingOrHoldingAndActiveResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::conferenceResponse(int slotId, int responseType,
                             int serial, RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("conferenceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->conferenceResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("conferenceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::rejectCallResponse(int slotId, int responseType,
                             int serial, RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("rejectCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->rejectCallResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("rejectCallResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getLastCallFailCauseResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e, void *response,
                                       size_t responseLen) {
#if VDBG
    RLOGD("getLastCallFailCauseResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        LastCallFailCauseInfo info = {};
        info.vendorCause = hidl_string();
        if (response == NULL) {
            RLOGE("getCurrentCallsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else if (responseLen == sizeof(RIL_LastCallFailCauseInfo))  {
            RIL_LastCallFailCauseInfo *pFailCauseInfo = (RIL_LastCallFailCauseInfo *) response;
            info.causeCode = (LastCallFailCause) pFailCauseInfo->cause_code;
            info.vendorCause = convertCharPtrToHidlString(pFailCauseInfo->vendor_cause);
        } else if (responseLen % sizeof(int) != 0) {
            int *pInt = (int *) response;
            info.causeCode = (LastCallFailCause) pInt[0];
        } else {
            RLOGE("getCurrentCallsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->getLastCallFailCauseResponse(
                responseInfo, info);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getLastCallFailCauseResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSignalStrengthResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getSignalStrengthResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        // Samsung blobs return one of four legacy layouts depending on modem generation.
        bool valid = response != NULL && (responseLen == sizeof(RIL_SignalStrength_v10)
                || responseLen == sizeof(RIL_SignalStrength_v8)
                || responseLen == sizeof(RIL_SignalStrength_v6)
                || responseLen == sizeof(RIL_SignalStrength_v5));
        if (!valid) {
            RLOGE("getSignalStrengthResponse: Invalid response");
        }

        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo = {};
            populateResponseInfo_1_6(responseInfo, serial, responseType, e);
            V1_6::SignalStrength signalStrength = {};
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = V1_6::RadioError::INVALID_RESPONSE;
            } else {
                convertRilSignalStrengthToHal_1_6(slotId, response, responseLen, signalStrength);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_6->
                    getSignalStrengthResponse_1_6(responseInfo, signalStrength);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            SignalStrength signalStrength = {};
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilSignalStrengthToHal(response, responseLen, signalStrength);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponse->
                    getSignalStrengthResponse(responseInfo, signalStrength);
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getSignalStrengthResponse: no radio response callback for slot %d", slotId);
    }

    return 0;
}

RIL_CellInfoType getCellInfoTypeRadioTechnology(char *rat) {
    if (rat == NULL) {
        return RIL_CELL_INFO_TYPE_NONE;
    }

    int radioTech = atoi(rat);

    switch(radioTech) {

        case RADIO_TECH_GPRS:
        case RADIO_TECH_EDGE:
        case RADIO_TECH_GSM: {
            return RIL_CELL_INFO_TYPE_GSM;
        }

        case RADIO_TECH_UMTS:
        case RADIO_TECH_HSDPA:
        case RADIO_TECH_HSUPA:
        case RADIO_TECH_HSPA:
        case RADIO_TECH_HSPAP: {
            return RIL_CELL_INFO_TYPE_WCDMA;
        }

        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD: {
            return RIL_CELL_INFO_TYPE_CDMA;
        }

        case RADIO_TECH_LTE:
        case RADIO_TECH_LTE_CA: {
            return RIL_CELL_INFO_TYPE_LTE;
        }

        case RADIO_TECH_TD_SCDMA: {
            return RIL_CELL_INFO_TYPE_TD_SCDMA;
        }

        default: {
            break;
        }
    }

    return RIL_CELL_INFO_TYPE_NONE;

}

void fillCellIdentityResponse(CellIdentity &cellIdentity, RIL_CellIdentity_v16 &rilCellIdentity) {

    cellIdentity.cellIdentityGsm.resize(0);
    cellIdentity.cellIdentityWcdma.resize(0);
    cellIdentity.cellIdentityCdma.resize(0);
    cellIdentity.cellIdentityTdscdma.resize(0);
    cellIdentity.cellIdentityLte.resize(0);
    cellIdentity.cellInfoType = (CellInfoType)rilCellIdentity.cellInfoType;
    switch(rilCellIdentity.cellInfoType) {

        case RIL_CELL_INFO_TYPE_GSM: {
            cellIdentity.cellIdentityGsm.resize(1);
            cellIdentity.cellIdentityGsm[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityGsm.mcc);
            cellIdentity.cellIdentityGsm[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityGsm.mnc);

            if (cellIdentity.cellIdentityGsm[0].mcc == "-1") {
                cellIdentity.cellIdentityGsm[0].mcc = "";
            }

            cellIdentity.cellIdentityGsm[0].lac = rilCellIdentity.cellIdentityGsm.lac;
            cellIdentity.cellIdentityGsm[0].cid = rilCellIdentity.cellIdentityGsm.cid;
            cellIdentity.cellIdentityGsm[0].arfcn = rilCellIdentity.cellIdentityGsm.arfcn;
            cellIdentity.cellIdentityGsm[0].bsic = rilCellIdentity.cellIdentityGsm.bsic;
            break;
        }

        case RIL_CELL_INFO_TYPE_WCDMA: {
            cellIdentity.cellIdentityWcdma.resize(1);
            cellIdentity.cellIdentityWcdma[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityWcdma.mcc);
            cellIdentity.cellIdentityWcdma[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityWcdma.mnc);

            if (cellIdentity.cellIdentityWcdma[0].mcc == "-1") {
                cellIdentity.cellIdentityWcdma[0].mcc = "";
            }

            cellIdentity.cellIdentityWcdma[0].lac = rilCellIdentity.cellIdentityWcdma.lac;
            cellIdentity.cellIdentityWcdma[0].cid = rilCellIdentity.cellIdentityWcdma.cid;
            cellIdentity.cellIdentityWcdma[0].psc = rilCellIdentity.cellIdentityWcdma.psc;
            cellIdentity.cellIdentityWcdma[0].uarfcn = rilCellIdentity.cellIdentityWcdma.uarfcn;
            break;
        }

        case RIL_CELL_INFO_TYPE_CDMA: {
            cellIdentity.cellIdentityCdma.resize(1);
            cellIdentity.cellIdentityCdma[0].networkId = rilCellIdentity.cellIdentityCdma.networkId;
            cellIdentity.cellIdentityCdma[0].systemId = rilCellIdentity.cellIdentityCdma.systemId;
            cellIdentity.cellIdentityCdma[0].baseStationId =
                    rilCellIdentity.cellIdentityCdma.basestationId;
            cellIdentity.cellIdentityCdma[0].longitude = rilCellIdentity.cellIdentityCdma.longitude;
            cellIdentity.cellIdentityCdma[0].latitude = rilCellIdentity.cellIdentityCdma.latitude;
            break;
        }

        case RIL_CELL_INFO_TYPE_LTE: {
            cellIdentity.cellIdentityLte.resize(1);
            cellIdentity.cellIdentityLte[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityLte.mcc);
            cellIdentity.cellIdentityLte[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityLte.mnc);

            if (cellIdentity.cellIdentityLte[0].mcc == "-1") {
                cellIdentity.cellIdentityLte[0].mcc = "";
            }

            cellIdentity.cellIdentityLte[0].ci = rilCellIdentity.cellIdentityLte.ci;
            cellIdentity.cellIdentityLte[0].pci = rilCellIdentity.cellIdentityLte.pci;
            cellIdentity.cellIdentityLte[0].tac = rilCellIdentity.cellIdentityLte.tac;
            cellIdentity.cellIdentityLte[0].earfcn = rilCellIdentity.cellIdentityLte.earfcn;
            break;
        }

        case RIL_CELL_INFO_TYPE_TD_SCDMA: {
            cellIdentity.cellIdentityTdscdma.resize(1);
            cellIdentity.cellIdentityTdscdma[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityTdscdma.mcc);
            cellIdentity.cellIdentityTdscdma[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityTdscdma.mnc);

            if (cellIdentity.cellIdentityTdscdma[0].mcc == "-1") {
                cellIdentity.cellIdentityTdscdma[0].mcc = "";
            }

            cellIdentity.cellIdentityTdscdma[0].lac = rilCellIdentity.cellIdentityTdscdma.lac;
            cellIdentity.cellIdentityTdscdma[0].cid = rilCellIdentity.cellIdentityTdscdma.cid;
            cellIdentity.cellIdentityTdscdma[0].cpid = rilCellIdentity.cellIdentityTdscdma.cpid;
            break;
        }

        default: {
            break;
        }
    }
}

int convertResponseStringEntryToInt(char **response, int index, int numStrings) {
    if ((response != NULL) &&  (numStrings > index) && (response[index] != NULL)) {
        return atoi(response[index]);
    }

    return -1;
}

int convertResponseHexStringEntryToInt(char **response, int index, int numStrings) {
    const int hexBase = 16;
    if ((response != NULL) &&  (numStrings > index) && (response[index] != NULL)) {
        return strtol(response[index], NULL, hexBase);
    }

    return -1;
}

/* Fill Cell Identity info from Voice Registration State Response.
 * This fucntion is applicable only for RIL Version < 15.
 * Response is a  "char **".
 * First and Second entries are in hex string format
 * and rest are integers represented in ascii format. */
void fillCellIdentityFromVoiceRegStateResponseString(CellIdentity &cellIdentity,
        int numStrings, char** response) {

    RIL_CellIdentity_v16 rilCellIdentity;
    memset(&rilCellIdentity, -1, sizeof(RIL_CellIdentity_v16));

    rilCellIdentity.cellInfoType = getCellInfoTypeRadioTechnology(response[3]);
    switch(rilCellIdentity.cellInfoType) {

        case RIL_CELL_INFO_TYPE_GSM: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityGsm.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityGsm.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_WCDMA: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityWcdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityWcdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            rilCellIdentity.cellIdentityWcdma.psc =
                    convertResponseStringEntryToInt(response, 14, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_TD_SCDMA:{
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityTdscdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityTdscdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_CDMA:{
            rilCellIdentity.cellIdentityCdma.basestationId =
                    convertResponseStringEntryToInt(response, 4, numStrings);
            /* Order of Lat. and Long. swapped between RIL and HIDL interface versions. */
            rilCellIdentity.cellIdentityCdma.latitude =
                    convertResponseStringEntryToInt(response, 5, numStrings);
            rilCellIdentity.cellIdentityCdma.longitude =
                    convertResponseStringEntryToInt(response, 6, numStrings);
            rilCellIdentity.cellIdentityCdma.systemId =
                    convertResponseStringEntryToInt(response, 8, numStrings);
            rilCellIdentity.cellIdentityCdma.networkId =
                    convertResponseStringEntryToInt(response, 9, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_LTE:{
            /* valid TAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityLte.tac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityLte.ci =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            break;
        }

        default: {
            break;
        }
    }

    fillCellIdentityResponse(cellIdentity, rilCellIdentity);
}

/* Fill Cell Identity info from Data Registration State Response.
 * This fucntion is applicable only for RIL Version < 15.
 * Response is a  "char **".
 * First and Second entries are in hex string format
 * and rest are integers represented in ascii format. */
void fillCellIdentityFromDataRegStateResponseString(CellIdentity &cellIdentity,
        int numStrings, char** response) {

    RIL_CellIdentity_v16 rilCellIdentity;
    memset(&rilCellIdentity, -1, sizeof(RIL_CellIdentity_v16));

    rilCellIdentity.cellInfoType = getCellInfoTypeRadioTechnology(response[3]);
    switch(rilCellIdentity.cellInfoType) {
        case RIL_CELL_INFO_TYPE_GSM: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityGsm.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityGsm.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityGsm.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityGsm.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            }
            break;
        }
        case RIL_CELL_INFO_TYPE_WCDMA: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityWcdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityWcdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityWcdma.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityWcdma.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            }
            break;
        }
        case RIL_CELL_INFO_TYPE_TD_SCDMA:{
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityTdscdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityTdscdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityTdscdma.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityTdscdma.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            }
            break;
        }
        case RIL_CELL_INFO_TYPE_LTE: {
            rilCellIdentity.cellIdentityLte.tac =
                    convertResponseStringEntryToInt(response, 6, numStrings);
            rilCellIdentity.cellIdentityLte.pci =
                    convertResponseStringEntryToInt(response, 7, numStrings);
            rilCellIdentity.cellIdentityLte.ci =
                    convertResponseStringEntryToInt(response, 8, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityLte.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityLte.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            }
            break;
        }
        default: {
            break;
        }
    }

    fillCellIdentityResponse(cellIdentity, rilCellIdentity);
}

int radio::getVoiceRegistrationStateResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("getVoiceRegistrationStateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        VoiceRegStateResult voiceRegResponse = {};
        bool valid = true;
        int numStrings = responseLen / sizeof(char *);
        if (response == NULL) {
               RLOGE("getVoiceRegistrationStateResponse Invalid response: NULL");
               if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
               valid = false;
        } else if (s_vendorFunctions->version <= 14) {
            if (numStrings < 15) {
                RLOGE("getVoiceRegistrationStateResponse Invalid response: NULL");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                valid = false;
            } else {
                char **resp = (char **) response;
                voiceRegResponse.regState = (RegState) ATOI_NULL_HANDLED_DEF(resp[0], 4);
                voiceRegResponse.rat = ATOI_NULL_HANDLED(resp[3]);
                voiceRegResponse.cssSupported = ATOI_NULL_HANDLED_DEF(resp[7], 0);
                voiceRegResponse.roamingIndicator = ATOI_NULL_HANDLED(resp[10]);
                voiceRegResponse.systemIsInPrl = ATOI_NULL_HANDLED_DEF(resp[11], 0);
                voiceRegResponse.defaultRoamingIndicator = ATOI_NULL_HANDLED_DEF(resp[12], 0);
                voiceRegResponse.reasonForDenial = ATOI_NULL_HANDLED_DEF(resp[13], 0);
                fillCellIdentityFromVoiceRegStateResponseString(voiceRegResponse.cellIdentity,
                        15, resp);
            }
        } else {
            RIL_VoiceRegistrationStateResponse *voiceRegState =
                    (RIL_VoiceRegistrationStateResponse *)response;

            if (responseLen != sizeof(RIL_VoiceRegistrationStateResponse)) {
                RLOGE("getVoiceRegistrationStateResponse Invalid response: NULL");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                valid = false;
            } else {
                voiceRegResponse.regState = (RegState) voiceRegState->regState;
                voiceRegResponse.rat = voiceRegState->rat;;
                voiceRegResponse.cssSupported = voiceRegState->cssSupported;
                voiceRegResponse.roamingIndicator = voiceRegState->roamingIndicator;
                voiceRegResponse.systemIsInPrl = voiceRegState->systemIsInPrl;
                voiceRegResponse.defaultRoamingIndicator = voiceRegState->defaultRoamingIndicator;
                voiceRegResponse.reasonForDenial = voiceRegState->reasonForDenial;
                fillCellIdentityResponse(voiceRegResponse.cellIdentity,
                        voiceRegState->cellIdentity);
            }
        }

        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo16 = {};
            populateResponseInfo_1_6(responseInfo16, serial, responseType, e);
            if (!valid && e == RIL_E_SUCCESS) {
                responseInfo16.error = V1_6::RadioError::INVALID_RESPONSE;
            }
            V1_6::RegStateResult result = {};
            if (valid) {
                convertVoiceRegStateToHal_1_6(slotId, voiceRegResponse, result);
                RLOGI("HIDL 1.6 voice registration: slot=%d state=%d rat=%d plmn=%s",
                        slotId, static_cast<int>(result.regState),
                        static_cast<int>(result.rat), result.registeredPlmn.c_str());
            }
            retStatus = radioService[slotId]->mRadioResponseV1_6->
                    getVoiceRegistrationStateResponse_1_6(responseInfo16, result);
        } else {
            retStatus = radioService[slotId]->mRadioResponse->getVoiceRegistrationStateResponse(
                    responseInfo, voiceRegResponse);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getVoiceRegistrationStateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getDataRegistrationStateResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getDataRegistrationStateResponse: serial %d", serial);
#endif
    char value[PROPERTY_VALUE_MAX];
    int nstrings;
    property_get("ro.ril.telephony.nstrings", value, "6");
    nstrings = atoi(value);

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        DataRegStateResult dataRegResponse = {};
        bool valid = true;
        if (response == NULL) {
            RLOGE("getDataRegistrationStateResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            valid = false;
        } else if (s_vendorFunctions->version <= 14) {
            int numStrings = responseLen / sizeof(char *);
            if ((numStrings != 6) && (numStrings != 11) && (numStrings != nstrings)) {
                RLOGE("getDataRegistrationStateResponse Invalid response: NULL");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                valid = false;
            } else {
                char **resp = (char **) response;
                dataRegResponse.regState = (RegState) ATOI_NULL_HANDLED_DEF(resp[0], 4);
                dataRegResponse.rat =  ATOI_NULL_HANDLED_DEF(resp[3], 0);
                dataRegResponse.reasonDataDenied = numStrings > 4
                        ? ATOI_NULL_HANDLED(resp[4]) : 0;
                dataRegResponse.maxDataCalls = numStrings > 5
                        ? ATOI_NULL_HANDLED_DEF(resp[5], 1) : 1;
                fillCellIdentityFromDataRegStateResponseString(dataRegResponse.cellIdentity,
                        numStrings, resp);
            }
        } else {
            RIL_DataRegistrationStateResponse *dataRegState =
                    (RIL_DataRegistrationStateResponse *)response;

            if (responseLen != sizeof(RIL_DataRegistrationStateResponse)) {
                RLOGE("getDataRegistrationStateResponse Invalid response: NULL");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                valid = false;
            } else {
                dataRegResponse.regState = (RegState) dataRegState->regState;
                dataRegResponse.rat = dataRegState->rat;;
                dataRegResponse.reasonDataDenied = dataRegState->reasonDataDenied;
                dataRegResponse.maxDataCalls = dataRegState->maxDataCalls;
                fillCellIdentityResponse(dataRegResponse.cellIdentity, dataRegState->cellIdentity);
            }
        }

        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo16 = {};
            populateResponseInfo_1_6(responseInfo16, serial, responseType, e);
            if (!valid && e == RIL_E_SUCCESS) {
                responseInfo16.error = V1_6::RadioError::INVALID_RESPONSE;
            }
            V1_6::RegStateResult result = {};
            if (valid) {
                const bool registered =
                        dataRegResponse.regState == RegState::REG_HOME ||
                        dataRegResponse.regState == RegState::REG_ROAMING;
                cacheDataRadioTechnology(slotId,
                        registered ? dataRegResponse.rat : RADIO_TECH_UNKNOWN);
                convertDataRegStateToHal_1_6(slotId, dataRegResponse, result);
                RLOGI("HIDL 1.6 data registration: slot=%d state=%d rat=%d plmn=%s",
                        slotId, static_cast<int>(result.regState),
                        static_cast<int>(result.rat), result.registeredPlmn.c_str());
            }
            retStatus = radioService[slotId]->mRadioResponseV1_6->
                    getDataRegistrationStateResponse_1_6(responseInfo16, result);
        } else {
            retStatus = radioService[slotId]->mRadioResponse->getDataRegistrationStateResponse(
                    responseInfo, dataRegResponse);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getDataRegistrationStateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getOperatorResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("getOperatorResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_string longName;
        hidl_string shortName;
        hidl_string numeric;
        int numStrings = responseLen / sizeof(char *);
        if (response == NULL || numStrings < 3) {
            RLOGE("getOperatorResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;

        } else {
            char **resp = (char **) response;
            longName = convertCharPtrToHidlString(resp[0]);
            shortName = convertCharPtrToHidlString(resp[1]);
            numeric = convertCharPtrToHidlString(resp[2]);
            cacheOperatorNumeric(slotId, resp[2]);
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getOperatorResponse(
                responseInfo, longName, shortName, numeric);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getOperatorResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setRadioPowerResponse(int slotId,
                                int responseType, int serial, RIL_Errno e, void *response,
                                size_t responseLen) {
    RLOGD("setRadioPowerResponse: serial %d", serial);

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo = {};
            populateResponseInfo_1_6(responseInfo, serial, responseType, e);
            retStatus = radioService[slotId]->mRadioResponseV1_6->setRadioPowerResponse_1_6(
                    responseInfo);
        } else {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            retStatus = radioService[slotId]->mRadioResponse->setRadioPowerResponse(responseInfo);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setRadioPowerResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendDtmfResponse(int slotId,
                           int responseType, int serial, RIL_Errno e, void *response,
                           size_t responseLen) {
#if VDBG
    RLOGD("sendDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->sendDtmfResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendDtmfResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

SendSmsResult makeSendSmsResult(RadioResponseInfo& responseInfo, int serial, int responseType,
                                RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    SendSmsResult result = {};

    if (response == NULL || responseLen != sizeof(RIL_SMS_Response)) {
        RLOGE("Invalid response: NULL");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        result.ackPDU = hidl_string();
    } else {
        RIL_SMS_Response *resp = (RIL_SMS_Response *) response;
        result.messageRef = resp->messageRef;
        result.ackPDU = convertCharPtrToHidlString(resp->ackPDU);
        result.errorCode = resp->errorCode;
    }
    return result;
}

int radio::sendSmsResponse(int slotId,
                          int responseType, int serial, RIL_Errno e, void *response,
                          size_t responseLen) {
#if VDBG
    RLOGD("sendSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);
        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo16 = {};
            populateResponseInfo_1_6(responseInfo16, serial, responseType, e);
            if (responseInfo.error == RadioError::INVALID_RESPONSE && e == RIL_E_SUCCESS) {
                responseInfo16.error = V1_6::RadioError::INVALID_RESPONSE;
            }
            retStatus = radioService[slotId]->mRadioResponseV1_6->sendSmsResponse_1_6(
                    responseInfo16, result);
        } else {
            retStatus = radioService[slotId]->mRadioResponse->sendSmsResponse(responseInfo, result);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendSmsResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendSMSExpectMoreResponse(int slotId,
                                    int responseType, int serial, RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("sendSMSExpectMoreResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);
        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo16 = {};
            populateResponseInfo_1_6(responseInfo16, serial, responseType, e);
            if (responseInfo.error == RadioError::INVALID_RESPONSE && e == RIL_E_SUCCESS) {
                responseInfo16.error = V1_6::RadioError::INVALID_RESPONSE;
            }
            retStatus = radioService[slotId]->mRadioResponseV1_6->sendSmsExpectMoreResponse_1_6(
                    responseInfo16, result);
        } else {
            retStatus = radioService[slotId]->mRadioResponse->sendSMSExpectMoreResponse(
                    responseInfo, result);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendSMSExpectMoreResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setupDataCallResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("setupDataCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        bool valid = response != NULL && (responseLen == sizeof(RIL_Data_Call_Response_v11)
                || responseLen == sizeof(RIL_Data_Call_Response_v9)
                || responseLen == sizeof(RIL_Data_Call_Response_v6));
        if (!valid) {
            if (response != NULL) RLOGE("setupDataCallResponse: Invalid response");
        }

        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo = {};
            populateResponseInfo_1_6(responseInfo, serial, responseType, e);
            V1_6::SetupDataCallResult result = {};
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = V1_6::RadioError::INVALID_RESPONSE;
                result.cause = V1_6::DataCallFailCause::ERROR_UNSPECIFIED;
                // No modem payload means no suggested delay, not an immediate retry.
                result.suggestedRetryTime = -1;
                result.type = V1_4::PdpProtocolType::UNKNOWN;
            } else {
                convertRilDataCallToHal_1_6(response, responseLen, result);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_6->
                    setupDataCallResponse_1_6(responseInfo, result);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            SetupDataCallResult result = {};
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                result.status = DataCallFailCause::ERROR_UNSPECIFIED;
                result.suggestedRetryTime = -1;
                result.type = hidl_string();
                result.ifname = hidl_string();
                result.addresses = hidl_string();
                result.dnses = hidl_string();
                result.gateways = hidl_string();
                result.pcscf = hidl_string();
            } else if (responseLen == sizeof(RIL_Data_Call_Response_v11)) {
                convertRilDataCallToHal((RIL_Data_Call_Response_v11 *) response, result);
            } else if (responseLen == sizeof(RIL_Data_Call_Response_v9)) {
                convertRilDataCallToHal((RIL_Data_Call_Response_v9 *) response, result);
            } else {
                convertRilDataCallToHal((RIL_Data_Call_Response_v6 *) response, result);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponse->setupDataCallResponse(
                    responseInfo, result);
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("setupDataCallResponse: no radio response callback for slot %d", slotId);
    }

    return 0;
}

IccIoResult responseIccIo(RadioResponseInfo& responseInfo, int serial, int responseType,
                           RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    IccIoResult result = {};

    if (response == NULL || responseLen != sizeof(RIL_SIM_IO_Response)) {
        RLOGE("Invalid response: NULL");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        result.simResponse = hidl_string();
    } else {
        RIL_SIM_IO_Response *resp = (RIL_SIM_IO_Response *) response;
        result.sw1 = resp->sw1;
        result.sw2 = resp->sw2;
        result.simResponse = convertCharPtrToHidlString(resp->simResponse);
    }
    return result;
}

int radio::iccIOForAppResponse(int slotId,
                      int responseType, int serial, RIL_Errno e, void *response,
                      size_t responseLen) {
#if VDBG
    RLOGD("iccIOForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        bool isIccidRead = false;
        {
            std::lock_guard<std::mutex> lock(sIccidCacheMutex);
            isIccidRead = sPendingIccidReads.erase({slotId, serial}) != 0;
        }

        bool iccidChanged = false;
        if (isIccidRead && e == RIL_E_SUCCESS && response != nullptr &&
                responseLen == sizeof(RIL_SIM_IO_Response)) {
            const RIL_SIM_IO_Response& simIo =
                    *static_cast<const RIL_SIM_IO_Response *>(response);
            std::string decodedIccid;
            if (isSuccessfulIccIo(simIo) && decodeEfIccid(simIo.simResponse, &decodedIccid)) {
                std::lock_guard<std::mutex> lock(sIccidCacheMutex);
                std::string& cached = sCachedIccids[slotId];
                if (cached != decodedIccid) {
                    cached = std::move(decodedIccid);
                    iccidChanged = true;
                }
            } else {
                RLOGW("iccIOForAppResponse: invalid EF_ICCID response on slot %d", slotId);
            }
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->iccIOForAppResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);

        if (iccidChanged) {
            RLOGI("iccIOForAppResponse: cached verified EF_ICCID (%zu digits) on slot %d; "
                    "refreshing card status", getCachedIccid(slotId).size(), slotId);
            simStatusChangedInd(slotId, RESPONSE_UNSOLICITED, 0, RIL_E_SUCCESS, nullptr, 0);
        }
    } else {
        RLOGE("iccIOForAppResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendUssdResponse(int slotId,
                           int responseType, int serial, RIL_Errno e, void *response,
                           size_t responseLen) {
#if VDBG
    RLOGD("sendUssdResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->sendUssdResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendUssdResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::cancelPendingUssdResponse(int slotId,
                                    int responseType, int serial, RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("cancelPendingUssdResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->cancelPendingUssdResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cancelPendingUssdResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getClirResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("getClirResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        int n = -1, m = -1;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts < 2) {
            RLOGE("getClirResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            n = pInt[0];
            m = pInt[1];
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getClirResponse(responseInfo,
                n, m);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getClirResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setClirResponse(int slotId,
                          int responseType, int serial, RIL_Errno e, void *response,
                          size_t responseLen) {
#if VDBG
    RLOGD("setClirResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setClirResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setClirResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getCallForwardStatusResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCallForwardStatusResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<CallForwardInfo> callForwardInfos;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_CallForwardInfo *) != 0) {
            RLOGE("getCallForwardStatusResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_CallForwardInfo *);
            callForwardInfos.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_CallForwardInfo *resp = ((RIL_CallForwardInfo **) response)[i];
                callForwardInfos[i].status = (CallForwardInfoStatus) resp->status;
                callForwardInfos[i].reason = resp->reason;
                callForwardInfos[i].serviceClass = resp->serviceClass;
                callForwardInfos[i].toa = resp->toa;
                callForwardInfos[i].number = convertCharPtrToHidlString(resp->number);
                callForwardInfos[i].timeSeconds = resp->timeSeconds;
            }
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->getCallForwardStatusResponse(
                responseInfo, callForwardInfos);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCallForwardStatusResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCallForwardResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("setCallForwardResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setCallForwardResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCallForwardResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getCallWaitingResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("getCallWaitingResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool enable = false;
        int serviceClass = -1;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts < 2) {
            RLOGE("getCallWaitingResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            enable = pInt[0] == 1 ? true : false;
            serviceClass = pInt[1];
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getCallWaitingResponse(
                responseInfo, enable, serviceClass);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCallWaitingResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setCallWaitingResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("setCallWaitingResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setCallWaitingResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCallWaitingResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::acknowledgeLastIncomingGsmSmsResponse(int slotId,
                                                int responseType, int serial, RIL_Errno e,
                                                void *response, size_t responseLen) {
#if VDBG
    RLOGD("acknowledgeLastIncomingGsmSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->acknowledgeLastIncomingGsmSmsResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeLastIncomingGsmSmsResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::acceptCallResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("acceptCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->acceptCallResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acceptCallResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::deactivateDataCallResponse(int slotId,
                                                int responseType, int serial, RIL_Errno e,
                                                void *response, size_t responseLen) {
#if VDBG
    RLOGD("deactivateDataCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->deactivateDataCallResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("deactivateDataCallResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getFacilityLockForAppResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("getFacilityLockForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                getFacilityLockForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getFacilityLockForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setFacilityLockForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("setFacilityLockForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setFacilityLockForAppResponse(responseInfo,
                ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setFacilityLockForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setBarringPasswordResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("acceptCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setBarringPasswordResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setBarringPasswordResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getNetworkSelectionModeResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e, void *response,
                                          size_t responseLen) {
#if VDBG
    RLOGD("getNetworkSelectionModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool manual = false;
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("getNetworkSelectionModeResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            manual = pInt[0] == 1 ? true : false;
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getNetworkSelectionModeResponse(
                responseInfo,
                manual);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getNetworkSelectionModeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setNetworkSelectionModeAutomaticResponse(int slotId, int responseType, int serial,
                                                    RIL_Errno e, void *response,
                                                    size_t responseLen) {
#if VDBG
    RLOGD("setNetworkSelectionModeAutomaticResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setNetworkSelectionModeAutomaticResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setNetworkSelectionModeAutomaticResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::setNetworkSelectionModeManualResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setNetworkSelectionModeManualResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_5 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_5 != NULL
                ? radioService[slotId]->mRadioResponseV1_5
                        ->setNetworkSelectionModeManualResponse_1_5(responseInfo)
                : radioService[slotId]->mRadioResponse
                        ->setNetworkSelectionModeManualResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acceptCallResponse: radioService[%d]->setNetworkSelectionModeManualResponse "
                "== NULL", slotId);
    }

    return 0;
}

int convertOperatorStatusToInt(const char *str) {
    if (strncmp("unknown", str, 9) == 0) {
        return (int) OperatorStatus::UNKNOWN;
    } else if (strncmp("available", str, 9) == 0) {
        return (int) OperatorStatus::AVAILABLE;
    } else if (strncmp("current", str, 9) == 0) {
        return (int) OperatorStatus::CURRENT;
    } else if (strncmp("forbidden", str, 9) == 0) {
        return (int) OperatorStatus::FORBIDDEN;
    } else {
        return -1;
    }
}

int radio::getAvailableNetworksResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("getAvailableNetworksResponse: serial %d", serial);
#endif
    int mqanelements;
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.ril.telephony.mqanelements", value, "4");
    mqanelements = atoi(value);

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<OperatorInfo> networks;
        if ((response == NULL && responseLen != 0)
                || responseLen % (mqanelements * sizeof(char *)) != 0) {
            RLOGE("getAvailableNetworksResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            char **resp = (char **) response;
            int numStrings = responseLen / sizeof(char *);
            networks.resize(numStrings/mqanelements);
            for (int i = 0, j = 0; i < numStrings; i = i + mqanelements, j++) {
                networks[j].alphaLong = convertCharPtrToHidlString(resp[i]);
                networks[j].alphaShort = convertCharPtrToHidlString(resp[i]);
                networks[j].operatorNumeric = convertCharPtrToHidlString(resp[i + 2]);
                int status = convertOperatorStatusToInt(resp[i + 3]);
                if (status == -1) {
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    networks[j].status = (OperatorStatus) status;
                }
            }
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getAvailableNetworksResponse(responseInfo,
                networks);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getAvailableNetworksResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::startDtmfResponse(int slotId,
                            int responseType, int serial, RIL_Errno e,
                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("startDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->startDtmfResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("startDtmfResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::stopDtmfResponse(int slotId,
                           int responseType, int serial, RIL_Errno e,
                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("stopDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->stopDtmfResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stopDtmfResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getBasebandVersionResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getBasebandVersionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getBasebandVersionResponse(responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getBasebandVersionResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::separateConnectionResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("separateConnectionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->separateConnectionResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("separateConnectionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setMuteResponse(int slotId,
                          int responseType, int serial, RIL_Errno e,
                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("setMuteResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setMuteResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setMuteResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getMuteResponse(int slotId,
                          int responseType, int serial, RIL_Errno e, void *response,
                          size_t responseLen) {
#if VDBG
    RLOGD("getMuteResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool enable = false;
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("getMuteResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            enable = pInt[0] == 1 ? true : false;
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getMuteResponse(responseInfo,
                enable);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getMuteResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getClipResponse(int slotId,
                          int responseType, int serial, RIL_Errno e,
                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getClipResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getClipResponse(responseInfo,
                (ClipStatus) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getClipResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getDataCallListResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("getDataCallListResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        bool valid = !((response == NULL && responseLen != 0)
                || (responseLen % sizeof(RIL_Data_Call_Response_v11) != 0
                && responseLen % sizeof(RIL_Data_Call_Response_v9) != 0
                && responseLen % sizeof(RIL_Data_Call_Response_v6) != 0));
        if (!valid) {
            RLOGE("getDataCallListResponse: invalid response");
        }

        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo = {};
            populateResponseInfo_1_6(responseInfo, serial, responseType, e);
            hidl_vec<V1_6::SetupDataCallResult> result;
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = V1_6::RadioError::INVALID_RESPONSE;
            } else {
                convertRilDataCallListToHal_1_6(response, responseLen, result);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_6->
                    getDataCallListResponse_1_6(responseInfo, result);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            hidl_vec<SetupDataCallResult> result;
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilDataCallListToHal(response, responseLen, result);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponse->getDataCallListResponse(
                    responseInfo, result);
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getDataCallListResponse: no radio response callback for slot %d", slotId);
    }

    return 0;
}

int radio::setSuppServiceNotificationsResponse(int slotId,
                                              int responseType, int serial, RIL_Errno e,
                                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSuppServiceNotificationsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setSuppServiceNotificationsResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setSuppServiceNotificationsResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::deleteSmsOnSimResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("deleteSmsOnSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->deleteSmsOnSimResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("deleteSmsOnSimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setBandModeResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("setBandModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setBandModeResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setBandModeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::writeSmsToSimResponse(int slotId,
                                int responseType, int serial, RIL_Errno e,
                                void *response, size_t responseLen) {
#if VDBG
    RLOGD("writeSmsToSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->writeSmsToSimResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("writeSmsToSimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getAvailableBandModesResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e, void *response,
                                        size_t responseLen) {
#if VDBG
    RLOGD("getAvailableBandModesResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<RadioBandMode> modes;
        if ((response == NULL && responseLen != 0)|| responseLen % sizeof(int) != 0) {
            RLOGE("getAvailableBandModesResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            int numInts = responseLen / sizeof(int);
            modes.resize(numInts);
            for (int i = 0; i < numInts; i++) {
                modes[i] = (RadioBandMode) pInt[i];
            }
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getAvailableBandModesResponse(responseInfo,
                modes);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getAvailableBandModesResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendEnvelopeResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendEnvelopeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendEnvelopeResponse(responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendEnvelopeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendTerminalResponseToSimResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendTerminalResponseToSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendTerminalResponseToSimResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendTerminalResponseToSimResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::handleStkCallSetupRequestFromSimResponse(int slotId,
                                                   int responseType, int serial,
                                                   RIL_Errno e, void *response,
                                                   size_t responseLen) {
#if VDBG
    RLOGD("handleStkCallSetupRequestFromSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->handleStkCallSetupRequestFromSimResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("handleStkCallSetupRequestFromSimResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::explicitCallTransferResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("explicitCallTransferResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->explicitCallTransferResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("explicitCallTransferResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setPreferredNetworkTypeResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setPreferredNetworkTypeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setPreferredNetworkTypeResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setPreferredNetworkTypeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}


int radio::getPreferredNetworkTypeResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getPreferredNetworkTypeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getPreferredNetworkTypeResponse(
                responseInfo, (PreferredNetworkType) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getPreferredNetworkTypeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setPreferredNetworkTypeBitmapResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *, size_t) {
    if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo info = {};
        populateResponseInfo(info, serial, responseType, e);
        Return<void> ret = radioService[slotId]->mRadioResponseV1_4
                ->setPreferredNetworkTypeBitmapResponse(info);
        radioService[slotId]->checkReturnStatus(ret);
    }
    return 0;
}

int radio::getPreferredNetworkTypeBitmapResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo info = {};
        int legacyType = responseInt(info, serial, responseType, e, response, responseLen);
        uint32_t raf = legacyNetworkTypeToRaf(legacyType);
        if (info.error == RadioError::NONE && raf == 0) info.error = RadioError::INVALID_RESPONSE;
        Return<void> ret = radioService[slotId]->mRadioResponseV1_4
                ->getPreferredNetworkTypeBitmapResponse(info, raf);
        radioService[slotId]->checkReturnStatus(ret);
    }
    return 0;
}

int radio::setAllowedNetworkTypesBitmapResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *, size_t) {
    if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
        V1_6::RadioResponseInfo info = {};
        populateResponseInfo_1_6(info, serial, responseType, e);
        Return<void> ret = radioService[slotId]->mRadioResponseV1_6
                ->setAllowedNetworkTypesBitmapResponse(info);
        radioService[slotId]->checkReturnStatus(ret);
    }
    return 0;
}

int radio::getAllowedNetworkTypesBitmapResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
        RadioResponseInfo legacyInfo = {};
        int legacyType = responseInt(
                legacyInfo, serial, responseType, e, response, responseLen);
        uint32_t raf = legacyNetworkTypeToRaf(legacyType);
        V1_6::RadioResponseInfo info = {};
        populateResponseInfo_1_6(info, serial, responseType, e);
        if (legacyInfo.error == RadioError::INVALID_RESPONSE ||
                (info.error == V1_6::RadioError::NONE && raf == 0)) {
            info.error = V1_6::RadioError::INVALID_RESPONSE;
        }
        Return<void> ret = radioService[slotId]->mRadioResponseV1_6
                ->getAllowedNetworkTypesBitmapResponse(info, raf);
        radioService[slotId]->checkReturnStatus(ret);
    }
    return 0;
}

int radio::getNeighboringCidsResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getNeighboringCidsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<NeighboringCell> cells;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_NeighboringCell *) != 0) {
            RLOGE("getNeighboringCidsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_NeighboringCell *);
            cells.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_NeighboringCell *resp = ((RIL_NeighboringCell **) response)[i];
                cells[i].cid = convertCharPtrToHidlString(resp->cid);
                cells[i].rssi = resp->rssi;
            }
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getNeighboringCidsResponse(responseInfo,
                cells);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getNeighboringCidsResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setLocationUpdatesResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("setLocationUpdatesResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setLocationUpdatesResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setLocationUpdatesResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaSubscriptionSourceResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaSubscriptionSourceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaSubscriptionSourceResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaSubscriptionSourceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaRoamingPreferenceResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaRoamingPreferenceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaRoamingPreferenceResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaRoamingPreferenceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCdmaRoamingPreferenceResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCdmaRoamingPreferenceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getCdmaRoamingPreferenceResponse(
                responseInfo, (CdmaRoamingType) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCdmaRoamingPreferenceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setTTYModeResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setTTYModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setTTYModeResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setTTYModeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getTTYModeResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("getTTYModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getTTYModeResponse(responseInfo,
                (TtyMode) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getTTYModeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setPreferredVoicePrivacyResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setPreferredVoicePrivacyResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setPreferredVoicePrivacyResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setPreferredVoicePrivacyResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getPreferredVoicePrivacyResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getPreferredVoicePrivacyResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool enable = false;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts < 1) {
            RLOGE("getPreferredVoicePrivacyResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            enable = pInt[0] == 1 ? true : false;
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getPreferredVoicePrivacyResponse(
                responseInfo, enable);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getPreferredVoicePrivacyResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendCDMAFeatureCodeResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendCDMAFeatureCodeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendCDMAFeatureCodeResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendCDMAFeatureCodeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendBurstDtmfResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendBurstDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendBurstDtmfResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendBurstDtmfResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendCdmaSmsResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("sendCdmaSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);
        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo16 = {};
            populateResponseInfo_1_6(responseInfo16, serial, responseType, e);
            if (responseInfo.error == RadioError::INVALID_RESPONSE && e == RIL_E_SUCCESS) {
                responseInfo16.error = V1_6::RadioError::INVALID_RESPONSE;
            }
            retStatus = radioService[slotId]->mRadioResponseV1_6->sendCdmaSmsResponse_1_6(
                    responseInfo16, result);
        } else {
            retStatus = radioService[slotId]->mRadioResponse->sendCdmaSmsResponse(
                    responseInfo, result);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendCdmaSmsResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendCdmaSmsExpectMoreResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponseV1_5 != NULL) {
        RadioResponseInfo legacyInfo = {};
        SendSmsResult result = makeSendSmsResult(
                legacyInfo, serial, responseType, e, response, responseLen);
        Return<void> ret;
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo info = {};
            populateResponseInfo_1_6(info, serial, responseType, e);
            if (legacyInfo.error == RadioError::INVALID_RESPONSE && e == RIL_E_SUCCESS) {
                info.error = V1_6::RadioError::INVALID_RESPONSE;
            }
            ret = radioService[slotId]->mRadioResponseV1_6
                    ->sendCdmaSmsExpectMoreResponse_1_6(info, result);
        } else {
            ret = radioService[slotId]->mRadioResponseV1_5
                    ->sendCdmaSmsExpectMoreResponse(legacyInfo, result);
        }
        radioService[slotId]->checkReturnStatus(ret);
    }
    return 0;
}

int radio::acknowledgeLastIncomingCdmaSmsResponse(int slotId,
                                                 int responseType, int serial, RIL_Errno e,
                                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("acknowledgeLastIncomingCdmaSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->acknowledgeLastIncomingCdmaSmsResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeLastIncomingCdmaSmsResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::getGsmBroadcastConfigResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("getGsmBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<GsmBroadcastSmsConfigInfo> configs;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_GSM_BroadcastSmsConfigInfo *) != 0) {
            RLOGE("getGsmBroadcastConfigResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_GSM_BroadcastSmsConfigInfo *);
            configs.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_GSM_BroadcastSmsConfigInfo *resp =
                        ((RIL_GSM_BroadcastSmsConfigInfo **) response)[i];
                configs[i].fromServiceId = resp->fromServiceId;
                configs[i].toServiceId = resp->toServiceId;
                configs[i].fromCodeScheme = resp->fromCodeScheme;
                configs[i].toCodeScheme = resp->toCodeScheme;
                configs[i].selected = resp->selected == 1 ? true : false;
            }
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getGsmBroadcastConfigResponse(responseInfo,
                configs);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getGsmBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setGsmBroadcastConfigResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("setGsmBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setGsmBroadcastConfigResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setGsmBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setGsmBroadcastActivationResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("setGsmBroadcastActivationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setGsmBroadcastActivationResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setGsmBroadcastActivationResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCdmaBroadcastConfigResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e,
                                         void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCdmaBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<CdmaBroadcastSmsConfigInfo> configs;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_CDMA_BroadcastSmsConfigInfo *) != 0) {
            RLOGE("getCdmaBroadcastConfigResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_CDMA_BroadcastSmsConfigInfo *);
            configs.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_CDMA_BroadcastSmsConfigInfo *resp =
                        ((RIL_CDMA_BroadcastSmsConfigInfo **) response)[i];
                configs[i].serviceCategory = resp->service_category;
                configs[i].language = resp->language;
                configs[i].selected = resp->selected == 1 ? true : false;
            }
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getCdmaBroadcastConfigResponse(responseInfo,
                configs);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCdmaBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaBroadcastConfigResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e,
                                         void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaBroadcastConfigResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaBroadcastActivationResponse(int slotId,
                                             int responseType, int serial, RIL_Errno e,
                                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaBroadcastActivationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaBroadcastActivationResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaBroadcastActivationResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCDMASubscriptionResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("getCDMASubscriptionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        int numStrings = responseLen / sizeof(char *);
        hidl_string emptyString;
        if (response == NULL || numStrings < 5) {
            RLOGE("getOperatorResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getCDMASubscriptionResponse(
                    responseInfo, emptyString, emptyString, emptyString, emptyString, emptyString);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            char **resp = (char **) response;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getCDMASubscriptionResponse(
                    responseInfo,
                    convertCharPtrToHidlString(resp[0]),
                    convertCharPtrToHidlString(resp[1]),
                    convertCharPtrToHidlString(resp[2]),
                    convertCharPtrToHidlString(resp[3]),
                    convertCharPtrToHidlString(resp[4]));
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getCDMASubscriptionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::writeSmsToRuimResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("writeSmsToRuimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->writeSmsToRuimResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("writeSmsToRuimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::deleteSmsOnRuimResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("deleteSmsOnRuimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->deleteSmsOnRuimResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("deleteSmsOnRuimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getDeviceIdentityResponse(int slotId,
                                    int responseType, int serial, RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("getDeviceIdentityResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        int numStrings = responseLen / sizeof(char *);
        hidl_string emptyString;
        if (response == NULL || numStrings < 4) {
            RLOGE("getDeviceIdentityResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getDeviceIdentityResponse(responseInfo,
                    emptyString, emptyString, emptyString, emptyString);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            char **resp = (char **) response;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getDeviceIdentityResponse(responseInfo,
                    convertCharPtrToHidlString(resp[0]),
                    convertCharPtrToHidlString(resp[1]),
                    convertCharPtrToHidlString(resp[2]),
                    convertCharPtrToHidlString(resp[3]));
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getDeviceIdentityResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::exitEmergencyCallbackModeResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("exitEmergencyCallbackModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->exitEmergencyCallbackModeResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("exitEmergencyCallbackModeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSmscAddressResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("getSmscAddressResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getSmscAddressResponse(responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getSmscAddressResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setSmscAddressResponse(int slotId,
                                             int responseType, int serial, RIL_Errno e,
                                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSmscAddressResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setSmscAddressResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setSmscAddressResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::reportSmsMemoryStatusResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("reportSmsMemoryStatusResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->reportSmsMemoryStatusResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("reportSmsMemoryStatusResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::reportStkServiceIsRunningResponse(int slotId,
                                             int responseType, int serial, RIL_Errno e,
                                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("reportStkServiceIsRunningResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                reportStkServiceIsRunningResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("reportStkServiceIsRunningResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCdmaSubscriptionSourceResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCdmaSubscriptionSourceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getCdmaSubscriptionSourceResponse(
                responseInfo, (CdmaSubscriptionSource) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCdmaSubscriptionSourceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::requestIsimAuthenticationResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("requestIsimAuthenticationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->requestIsimAuthenticationResponse(
                responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("requestIsimAuthenticationResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::acknowledgeIncomingGsmSmsWithPduResponse(int slotId,
                                                   int responseType,
                                                   int serial, RIL_Errno e, void *response,
                                                   size_t responseLen) {
#if VDBG
    RLOGD("acknowledgeIncomingGsmSmsWithPduResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->acknowledgeIncomingGsmSmsWithPduResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeIncomingGsmSmsWithPduResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::sendEnvelopeWithStatusResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e, void *response,
                                         size_t responseLen) {
#if VDBG
    RLOGD("sendEnvelopeWithStatusResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e,
                response, responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendEnvelopeWithStatusResponse(responseInfo,
                result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendEnvelopeWithStatusResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getVoiceRadioTechnologyResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getVoiceRadioTechnologyResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getVoiceRadioTechnologyResponse(
                responseInfo, (RadioTechnology) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getVoiceRadioTechnologyResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCellInfoListResponse(int slotId,
                                   int responseType,
                                   int serial, RIL_Errno e, void *response,
                                   size_t responseLen) {
#if VDBG
    RLOGD("getCellInfoListResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_6 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        bool valid = !((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_CellInfo_v12) != 0);
        if (!valid) {
            RLOGE("getCellInfoListResponse: Invalid response");
        }

        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo = {};
            populateResponseInfo_1_6(responseInfo, serial, responseType, e);
            hidl_vec<V1_6::CellInfo> result;
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = V1_6::RadioError::INVALID_RESPONSE;
            } else {
                convertRilCellInfoListToHal_1_6(slotId, response, responseLen, result);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_6->
                    getCellInfoListResponse_1_6(responseInfo, result);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            hidl_vec<CellInfo> result;
            if (!valid) {
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilCellInfoListToHal(response, responseLen, result);
            }
            Return<void> retStatus = radioService[slotId]->mRadioResponse->getCellInfoListResponse(
                    responseInfo, result);
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getCellInfoListResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setCellInfoListRateResponse(int slotId,
                                       int responseType,
                                       int serial, RIL_Errno e, void *response,
                                       size_t responseLen) {
#if VDBG
    RLOGD("setCellInfoListRateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCellInfoListRateResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCellInfoListRateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setInitialAttachApnResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("setInitialAttachApnResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_5 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_5 != NULL
                ? radioService[slotId]->mRadioResponseV1_5
                        ->setInitialAttachApnResponse_1_5(responseInfo)
                : radioService[slotId]->mRadioResponse
                        ->setInitialAttachApnResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setInitialAttachApnResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getImsRegistrationStateResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getImsRegistrationStateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool isRegistered = false;
        int ratFamily = 0;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts < 2) {
            RLOGE("getImsRegistrationStateResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            isRegistered = pInt[0] == 1 ? true : false;
            ratFamily = pInt[1];
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getImsRegistrationStateResponse(
                responseInfo, isRegistered, (RadioTechnologyFamily) ratFamily);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getImsRegistrationStateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendImsSmsResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("sendImsSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendImsSmsResponse(responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendSmsResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::iccTransmitApduBasicChannelResponse(int slotId,
                                               int responseType, int serial, RIL_Errno e,
                                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("iccTransmitApduBasicChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccTransmitApduBasicChannelResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccTransmitApduBasicChannelResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::iccOpenLogicalChannelResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e, void *response,
                                         size_t responseLen) {
#if VDBG
    RLOGD("iccOpenLogicalChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        int channelId = -1;
        hidl_vec<int8_t> selectResponse;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("iccOpenLogicalChannelResponse Invalid response: NULL");
            if (response != NULL) {
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            }
        } else {
            int *pInt = (int *) response;
            channelId = pInt[0];
            selectResponse.resize(numInts - 1);
            for (int i = 1; i < numInts; i++) {
                selectResponse[i - 1] = (int8_t) pInt[i];
            }
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccOpenLogicalChannelResponse(responseInfo,
                channelId, selectResponse);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccOpenLogicalChannelResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::iccCloseLogicalChannelResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("iccCloseLogicalChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccCloseLogicalChannelResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccCloseLogicalChannelResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::iccTransmitApduLogicalChannelResponse(int slotId,
                                                 int responseType, int serial, RIL_Errno e,
                                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("iccTransmitApduLogicalChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccTransmitApduLogicalChannelResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccTransmitApduLogicalChannelResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::nvReadItemResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvReadItemResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->nvReadItemResponse(
                responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvReadItemResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::nvWriteItemResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvWriteItemResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->nvWriteItemResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvWriteItemResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::nvWriteCdmaPrlResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvWriteCdmaPrlResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->nvWriteCdmaPrlResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvWriteCdmaPrlResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::nvResetConfigResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvResetConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->nvResetConfigResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvResetConfigResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setUiccSubscriptionResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("setUiccSubscriptionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setUiccSubscriptionResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setUiccSubscriptionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setDataAllowedResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("setDataAllowedResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setDataAllowedResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setDataAllowedResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getHardwareConfigResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getHardwareConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        hidl_vec<HardwareConfig> result;
        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_HardwareConfig) != 0) {
            RLOGE("hardwareConfigChangedInd: invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            convertRilHardwareConfigListToHal(response, responseLen, result);
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->getHardwareConfigResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getHardwareConfigResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::requestIccSimAuthenticationResponse(int slotId,
                                               int responseType, int serial, RIL_Errno e,
                                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("requestIccSimAuthenticationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->requestIccSimAuthenticationResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("requestIccSimAuthenticationResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::setDataProfileResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("setDataProfileResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_5 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_5 != NULL
                ? radioService[slotId]->mRadioResponseV1_5
                        ->setDataProfileResponse_1_5(responseInfo)
                : radioService[slotId]->mRadioResponse->setDataProfileResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setDataProfileResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::requestShutdownResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("requestShutdownResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->requestShutdownResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("requestShutdownResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

void responseRadioCapability(RadioResponseInfo& responseInfo, int serial,
        int responseType, RIL_Errno e, void *response, size_t responseLen, RadioCapability& rc) {
    populateResponseInfo(responseInfo, serial, responseType, e);

    if (response == NULL || responseLen != sizeof(RIL_RadioCapability)) {
        RLOGE("responseRadioCapability: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        rc.logicalModemUuid = hidl_string();
    } else {
        convertRilRadioCapabilityToHal(response, responseLen, rc);
    }
}

int radio::getRadioCapabilityResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getRadioCapabilityResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        RadioCapability result = {};
        responseRadioCapability(responseInfo, serial, responseType, e, response, responseLen,
                result);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getRadioCapabilityResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getRadioCapabilityResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setRadioCapabilityResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("setRadioCapabilityResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        RadioCapability result = {};
        responseRadioCapability(responseInfo, serial, responseType, e, response, responseLen,
                result);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setRadioCapabilityResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setRadioCapabilityResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

LceStatusInfo responseLceStatusInfo(RadioResponseInfo& responseInfo, int serial, int responseType,
                                    RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    LceStatusInfo result = {};

    if (response == NULL || responseLen != sizeof(RIL_LceStatusInfo)) {
        RLOGE("Invalid response: NULL");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        RIL_LceStatusInfo *resp = (RIL_LceStatusInfo *) response;
        result.lceStatus = (LceStatus) resp->lce_status;
        result.actualIntervalMs = (uint8_t) resp->actual_interval_ms;
    }
    return result;
}

int radio::startLceServiceResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("startLceServiceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        LceStatusInfo result = responseLceStatusInfo(responseInfo, serial, responseType, e,
                response, responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->startLceServiceResponse(responseInfo,
                result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("startLceServiceResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::stopLceServiceResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("stopLceServiceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        LceStatusInfo result = responseLceStatusInfo(responseInfo, serial, responseType, e,
                response, responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->stopLceServiceResponse(responseInfo,
                result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stopLceServiceResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::pullLceDataResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("pullLceDataResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        LceDataInfo result = {};
        if (response == NULL || responseLen != sizeof(RIL_LceDataInfo)) {
            RLOGE("pullLceDataResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            convertRilLceDataInfoToHal(response, responseLen, result);
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->pullLceDataResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("pullLceDataResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getModemActivityInfoResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("getModemActivityInfoResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        ActivityStatsInfo info;
        if (response == NULL || responseLen != sizeof(RIL_ActivityStatsInfo)) {
            RLOGE("getModemActivityInfoResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            RIL_ActivityStatsInfo *resp = (RIL_ActivityStatsInfo *)response;
            info.sleepModeTimeMs = resp->sleep_mode_time_ms;
            info.idleModeTimeMs = resp->idle_mode_time_ms;
            for(int i = 0; i < RIL_NUM_TX_POWER_LEVELS; i++) {
                info.txmModetimeMs[i] = resp->tx_mode_time_ms[i];
            }
            info.rxModeTimeMs = resp->rx_mode_time_ms;
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getModemActivityInfoResponse(responseInfo,
                info);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getModemActivityInfoResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setAllowedCarriersResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("setAllowedCarriersResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setAllowedCarriersResponse(responseInfo,
                ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setAllowedCarriersResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getAllowedCarriersResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("getAllowedCarriersResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        CarrierRestrictions carrierInfo = {};
        bool allAllowed = true;
        if (response == NULL) {
#if VDBG
            RLOGD("getAllowedCarriersResponse response is NULL: all allowed");
#endif
            carrierInfo.allowedCarriers.resize(0);
            carrierInfo.excludedCarriers.resize(0);
        } else if (responseLen != sizeof(RIL_CarrierRestrictions)) {
            RLOGE("getAllowedCarriersResponse Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            RIL_CarrierRestrictions *pCr = (RIL_CarrierRestrictions *)response;
            if (pCr->len_allowed_carriers > 0 || pCr->len_excluded_carriers > 0) {
                allAllowed = false;
            }

            carrierInfo.allowedCarriers.resize(pCr->len_allowed_carriers);
            for(int i = 0; i < pCr->len_allowed_carriers; i++) {
                RIL_Carrier *carrier = pCr->allowed_carriers + i;
                carrierInfo.allowedCarriers[i].mcc = convertCharPtrToHidlString(carrier->mcc);
                carrierInfo.allowedCarriers[i].mnc = convertCharPtrToHidlString(carrier->mnc);
                carrierInfo.allowedCarriers[i].matchType = (CarrierMatchType) carrier->match_type;
                carrierInfo.allowedCarriers[i].matchData =
                        convertCharPtrToHidlString(carrier->match_data);
            }

            carrierInfo.excludedCarriers.resize(pCr->len_excluded_carriers);
            for(int i = 0; i < pCr->len_excluded_carriers; i++) {
                RIL_Carrier *carrier = pCr->excluded_carriers + i;
                carrierInfo.excludedCarriers[i].mcc = convertCharPtrToHidlString(carrier->mcc);
                carrierInfo.excludedCarriers[i].mnc = convertCharPtrToHidlString(carrier->mnc);
                carrierInfo.excludedCarriers[i].matchType = (CarrierMatchType) carrier->match_type;
                carrierInfo.excludedCarriers[i].matchData =
                        convertCharPtrToHidlString(carrier->match_data);
            }
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getAllowedCarriersResponse(responseInfo,
                allAllowed, carrierInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getAllowedCarriersResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendDeviceStateResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responselen) {
#if VDBG
    RLOGD("sendDeviceStateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendDeviceStateResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendDeviceStateResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setCarrierInfoForImsiEncryptionResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
    RLOGD("setCarrierInfoForImsiEncryptionResponse: serial %d", serial);
    if (radioService[slotId]->mRadioResponseV1_1 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_1->
                setCarrierInfoForImsiEncryptionResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCarrierInfoForImsiEncryptionResponse: radioService[%d]->mRadioResponseV1_1 == "
                "NULL", slotId);
    }
    return 0;
}

int radio::setIndicationFilterResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responselen) {
#if VDBG
    RLOGD("setIndicationFilterResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_5 != NULL ||
            radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_5 != NULL
                ? radioService[slotId]->mRadioResponseV1_5
                        ->setIndicationFilterResponse_1_5(responseInfo)
                : radioService[slotId]->mRadioResponse
                        ->setIndicationFilterResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setIndicationFilterResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setSimCardPowerResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSimCardPowerResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL
            || radioService[slotId]->mRadioResponseV1_1 != NULL) {
        if (radioService[slotId]->mRadioResponseV1_6 != NULL) {
            V1_6::RadioResponseInfo responseInfo = {};
            populateResponseInfo_1_6(responseInfo, serial, responseType, e);
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_6->
                    setSimCardPowerResponse_1_6(responseInfo);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else if (radioService[slotId]->mRadioResponseV1_1 != NULL) {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_1->
                    setSimCardPowerResponse_1_1(responseInfo);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            RadioResponseInfo responseInfo = {};
            populateResponseInfo(responseInfo, serial, responseType, e);
            RLOGD("setSimCardPowerResponse: radioService[%d]->mRadioResponseV1_1 == NULL",
                    slotId);
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->setSimCardPowerResponse(responseInfo);
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("setSimCardPowerResponse: radioService[%d]->mRadioResponse == NULL && "
                "radioService[%d]->mRadioResponseV1_1 == NULL", slotId, slotId);
    }
    return 0;
}

int radio::enableUiccApplicationsResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
    (void)response;
    (void)responseLen;

#ifndef SAMSUNG_NEXT_GEN_MODEM
    finishUiccApplicationsRequest(slotId, serial, e == RIL_E_SUCCESS);
#endif

    if (radioService[slotId] == nullptr ||
            radioService[slotId]->mRadioResponseV1_5 == nullptr) {
        RLOGE("enableUiccApplicationsResponse: no HIDL 1.5 response callback for slot %d",
                slotId);
        return 0;
    }

    RadioResponseInfo responseInfo = {};
    populateResponseInfo(responseInfo, serial, responseType, e);
    Return<void> retStatus = radioService[slotId]->mRadioResponseV1_5
            ->enableUiccApplicationsResponse(responseInfo);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::startNetworkScanResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                    void *response, size_t responseLen) {
#if VDBG
    RLOGD("startNetworkScanResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_1 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponseV1_1->startNetworkScanResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("startNetworkScanResponse: radioService[%d]->mRadioResponseV1_1 == NULL", slotId);
    }

    return 0;
}

int radio::stopNetworkScanResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("stopNetworkScanResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_1 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponseV1_1->stopNetworkScanResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stopNetworkScanResponse: radioService[%d]->mRadioResponseV1_1 == NULL", slotId);
    }

    return 0;
}

void convertRilKeepaliveStatusToHal(const RIL_KeepaliveStatus *rilStatus,
        V1_1::KeepaliveStatus& halStatus) {
    halStatus.sessionHandle = rilStatus->sessionHandle;
    halStatus.code = static_cast<V1_1::KeepaliveStatusCode>(rilStatus->code);
}

int radio::startKeepaliveResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                    void *response, size_t responseLen) {
#if VDBG
    RLOGD("%s(): %d", __FUNCTION__, serial);
#endif
    RadioResponseInfo responseInfo = {};
    populateResponseInfo(responseInfo, serial, responseType, e);

    // If we don't have a radio service, there's nothing we can do
    if (radioService[slotId]->mRadioResponseV1_1 == NULL) {
        RLOGE("%s: radioService[%d]->mRadioResponseV1_1 == NULL", __FUNCTION__, slotId);
        return 0;
    }

    V1_1::KeepaliveStatus ks = {};
    if (response == NULL || responseLen != sizeof(V1_1::KeepaliveStatus)) {
        RLOGE("%s: invalid response - %d", __FUNCTION__, static_cast<int>(e));
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        convertRilKeepaliveStatusToHal(static_cast<RIL_KeepaliveStatus*>(response), ks);
    }

    Return<void> retStatus =
            radioService[slotId]->mRadioResponseV1_1->startKeepaliveResponse(responseInfo, ks);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::stopKeepaliveResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                    void *response, size_t responseLen) {
#if VDBG
    RLOGD("%s(): %d", __FUNCTION__, serial);
#endif
    RadioResponseInfo responseInfo = {};
    populateResponseInfo(responseInfo, serial, responseType, e);

    // If we don't have a radio service, there's nothing we can do
    if (radioService[slotId]->mRadioResponseV1_1 == NULL) {
        RLOGE("%s: radioService[%d]->mRadioResponseV1_1 == NULL", __FUNCTION__, slotId);
        return 0;
    }

    Return<void> retStatus =
            radioService[slotId]->mRadioResponseV1_1->stopKeepaliveResponse(responseInfo);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::sendRequestRawResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
   RLOGD("sendRequestRawResponse: serial %d", serial);
#endif

    if (!kOemHookEnabled) return 0;

    if (oemHookService[slotId]->mOemHookResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<uint8_t> data;

        if (response == NULL) {
            RLOGE("sendRequestRawResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            data.setToExternal((uint8_t *) response, responseLen);
        }
        Return<void> retStatus = oemHookService[slotId]->mOemHookResponse->
                sendRequestRawResponse(responseInfo, data);
        checkReturnStatus(slotId, retStatus, false);
    } else {
        RLOGE("sendRequestRawResponse: oemHookService[%d]->mOemHookResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendRequestStringsResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendRequestStringsResponse: serial %d", serial);
#endif

    if (!kOemHookEnabled) return 0;

    if (oemHookService[slotId]->mOemHookResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<hidl_string> data;

        if ((response == NULL && responseLen != 0) || responseLen % sizeof(char *) != 0) {
            RLOGE("sendRequestStringsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            char **resp = (char **) response;
            int numStrings = responseLen / sizeof(char *);
            data.resize(numStrings);
            for (int i = 0; i < numStrings; i++) {
                data[i] = convertCharPtrToHidlString(resp[i]);
            }
        }
        Return<void> retStatus
                = oemHookService[slotId]->mOemHookResponse->sendRequestStringsResponse(
                responseInfo, data);
        checkReturnStatus(slotId, retStatus, false);
    } else {
        RLOGE("sendRequestStringsResponse: oemHookService[%d]->mOemHookResponse == "
                "NULL", slotId);
    }

    return 0;
}

/***************************************************************************************************
 * INDICATION FUNCTIONS
 * The below function handle unsolicited messages coming from the Radio
 * (messages for which there is no pending request)
 **************************************************************************************************/

RadioIndicationType convertIntToRadioIndicationType(int indicationType) {
    return indicationType == RESPONSE_UNSOLICITED ? (RadioIndicationType::UNSOLICITED) :
            (RadioIndicationType::UNSOLICITED_ACK_EXP);
}

static int
decodeVoiceRadioTechnology (RIL_RadioState radioState) {
    switch (radioState) {
        case RADIO_STATE_SIM_NOT_READY:
        case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
        case RADIO_STATE_SIM_READY:
            return RADIO_TECH_UMTS;

        case RADIO_STATE_RUIM_NOT_READY:
        case RADIO_STATE_RUIM_READY:
        case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
        case RADIO_STATE_NV_NOT_READY:
        case RADIO_STATE_NV_READY:
            return RADIO_TECH_1xRTT;

        default:
            RLOGD("decodeVoiceRadioTechnology: Invoked with incorrect RadioState");
            return -1;
    }
}

static int
decodeCdmaSubscriptionSource (RIL_RadioState radioState) {
    switch (radioState) {
        case RADIO_STATE_SIM_NOT_READY:
        case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
        case RADIO_STATE_SIM_READY:
        case RADIO_STATE_RUIM_NOT_READY:
        case RADIO_STATE_RUIM_READY:
        case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
            return CDMA_SUBSCRIPTION_SOURCE_RUIM_SIM;

        case RADIO_STATE_NV_NOT_READY:
        case RADIO_STATE_NV_READY:
            return CDMA_SUBSCRIPTION_SOURCE_NV;

        default:
            RLOGD("decodeCdmaSubscriptionSource: Invoked with incorrect RadioState");
            return -1;
    }
}

static int
decodeSimStatus (RIL_RadioState radioState) {
   switch (radioState) {
       case RADIO_STATE_SIM_NOT_READY:
       case RADIO_STATE_RUIM_NOT_READY:
       case RADIO_STATE_NV_NOT_READY:
       case RADIO_STATE_NV_READY:
           return -1;
       case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
       case RADIO_STATE_SIM_READY:
       case RADIO_STATE_RUIM_READY:
       case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
           return radioState;
       default:
           RLOGD("decodeSimStatus: Invoked with incorrect RadioState");
           return -1;
   }
}

static bool is3gpp2(int radioTech) {
    switch (radioTech) {
        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD:
            return true;
        default:
            return false;
    }
}

/* If RIL sends SIM states or RUIM states, store the voice radio
 * technology and subscription source information so that they can be
 * returned when telephony framework requests them
 */
int radio::processRadioState(int newRadioState, int slotId, int indicationType, int token, RIL_Errno e) {
    RLOGE("%s: BEGIN : %d", __func__, newRadioState);
    if((newRadioState > RADIO_STATE_UNAVAILABLE) && (newRadioState < RADIO_STATE_ON)) {
        RLOGE("%s: Executing old RIL", __func__);
        int newVoiceRadioTech;
        int newCdmaSubscriptionSource;
        int newSimStatus;

        /* This is old RIL. Decode Subscription source and Voice Radio Technology
           from Radio State and send change notifications if there has been a change */
        newVoiceRadioTech = decodeVoiceRadioTechnology((RIL_RadioState)newRadioState);
        if(newVoiceRadioTech != voiceRadioTech) {
            voiceRadioTech = newVoiceRadioTech;
            voiceRadioTechChangedInd(slotId, indicationType, token, e, &voiceRadioTech, sizeof(int));
        }
        if(is3gpp2(newVoiceRadioTech)) {
            newCdmaSubscriptionSource = decodeCdmaSubscriptionSource((RIL_RadioState)newRadioState);
            if(newCdmaSubscriptionSource != cdmaSubscriptionSource) {
                cdmaSubscriptionSource = newCdmaSubscriptionSource;
                cdmaSubscriptionSourceChangedInd(slotId, indicationType, token, e, &cdmaSubscriptionSource, sizeof(int));
            }
        }
        newSimStatus = decodeSimStatus((RIL_RadioState)newRadioState);
        if(newSimStatus != simRuimStatus) {
            simRuimStatus = newSimStatus;
            simStatusChangedInd(slotId, indicationType, token, e, &simRuimStatus, sizeof(int));
        }

        /* Send RADIO_ON to telephony */
        newRadioState = RADIO_STATE_ON;
    }

    RLOGE("%s: END", __func__);
    return newRadioState;
}

int radio::radioStateChangedInd(int slotId,
                                 int indicationType, int token, RIL_Errno e, void *response,
                                 size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        RadioState radioState =
                (RadioState)processRadioState(CALL_ONSTATEREQUEST(slotId), slotId, indicationType, token, e);
        RLOGD("radioStateChangedInd: radioState %d", radioState);
        Return<void> retStatus = radioService[slotId]->mRadioIndication->radioStateChanged(
                convertIntToRadioIndicationType(indicationType), radioState);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("radioStateChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::callStateChangedInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("callStateChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->callStateChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("callStateChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::networkStateChangedInd(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
#ifndef SAMSUNG_NEXT_GEN_MODEM
    const int64_t nowMs = android::elapsedRealtime();
    {
        std::lock_guard<std::mutex> lock(sNetworkStateIndicationMutex);
        const auto it = sLastNetworkStateIndicationMs.find(slotId);
        if (it != sLastNetworkStateIndicationMs.end() &&
                nowMs - it->second < kNetworkStateIndicationCoalesceMs) {
            RLOGD("networkStateChangedInd: coalesced duplicate for slot %d", slotId);
            return 0;
        }
        sLastNetworkStateIndicationMs[slotId] = nowMs;
    }
#endif
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("networkStateChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->networkStateChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("networkStateChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

uint8_t hexCharToInt(uint8_t c) {
    if (c >= '0' && c <= '9') return (c - '0');
    if (c >= 'A' && c <= 'F') return (c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (c - 'a' + 10);

    return INVALID_HEX_CHAR;
}

uint8_t * convertHexStringToBytes(void *response, size_t responseLen) {
    if (responseLen % 2 != 0) {
        return NULL;
    }

    uint8_t *bytes = (uint8_t *)calloc(responseLen/2, sizeof(uint8_t));
    if (bytes == NULL) {
        RLOGE("convertHexStringToBytes: cannot allocate memory for bytes string");
        return NULL;
    }
    uint8_t *hexString = (uint8_t *)response;

    for (size_t i = 0; i < responseLen; i += 2) {
        uint8_t hexChar1 = hexCharToInt(hexString[i]);
        uint8_t hexChar2 = hexCharToInt(hexString[i + 1]);

        if (hexChar1 == INVALID_HEX_CHAR || hexChar2 == INVALID_HEX_CHAR) {
            RLOGE("convertHexStringToBytes: invalid hex char %d %d",
                    hexString[i], hexString[i + 1]);
            free(bytes);
            return NULL;
        }
        bytes[i/2] = ((hexChar1 << 4) | hexChar2);
    }

    return bytes;
}

int radio::newSmsInd(int slotId, int indicationType,
                     int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("newSmsInd: invalid response");
            return 0;
        }

        uint8_t *bytes = convertHexStringToBytes(response, responseLen);
        if (bytes == NULL) {
            RLOGE("newSmsInd: convertHexStringToBytes failed");
            return 0;
        }

        hidl_vec<uint8_t> pdu;
        pdu.setToExternal(bytes, responseLen/2);
#if VDBG
        RLOGD("newSmsInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newSms(
                convertIntToRadioIndicationType(indicationType), pdu);
        radioService[slotId]->checkReturnStatus(retStatus);
        free(bytes);
    } else {
        RLOGE("newSmsInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::newSmsStatusReportInd(int slotId,
                                 int indicationType, int token, RIL_Errno e, void *response,
                                 size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("newSmsStatusReportInd: invalid response");
            return 0;
        }

        uint8_t *bytes = convertHexStringToBytes(response, responseLen);
        if (bytes == NULL) {
            RLOGE("newSmsStatusReportInd: convertHexStringToBytes failed");
            return 0;
        }

        hidl_vec<uint8_t> pdu;
        pdu.setToExternal(bytes, responseLen/2);
#if VDBG
        RLOGD("newSmsStatusReportInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newSmsStatusReport(
                convertIntToRadioIndicationType(indicationType), pdu);
        radioService[slotId]->checkReturnStatus(retStatus);
        free(bytes);
    } else {
        RLOGE("newSmsStatusReportInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::newSmsOnSimInd(int slotId, int indicationType,
                          int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("newSmsOnSimInd: invalid response");
            return 0;
        }
        int32_t recordNumber = ((int32_t *) response)[0];
#if VDBG
        RLOGD("newSmsOnSimInd: slotIndex %d", recordNumber);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newSmsOnSim(
                convertIntToRadioIndicationType(indicationType), recordNumber);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("newSmsOnSimInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::onUssdInd(int slotId, int indicationType,
                     int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen < 2 * sizeof(char *)) {
            RLOGE("onUssdInd: invalid response");
            return 0;
        }
        char **strings = (char **) response;
        char *mode = strings[0];
        hidl_string msg = convertCharPtrToHidlString(strings[1]);
        UssdModeType modeType = (UssdModeType) atoi(mode);
#if VDBG
        RLOGD("onUssdInd: mode %s", mode);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->onUssd(
                convertIntToRadioIndicationType(indicationType), modeType, msg);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("onUssdInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::nitzTimeReceivedInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("nitzTimeReceivedInd: invalid response");
            return 0;
        }
        hidl_string nitzTime;
        char *resp = strndup((char *) response, responseLen);
        char *tmp = resp;

        /* Find the 3rd comma */
        for (int i = 0; i < 3; i++) {
            if (tmp != NULL) {
                tmp = strchr(tmp + 1, ',');
            }
        }

        /* Make the 3rd comma the end of the string */
        if (tmp != NULL) {
            *tmp = '\0';
        }

        nitzTime = convertCharPtrToHidlString(resp);
        free(resp);
#if VDBG
        RLOGD("nitzTimeReceivedInd: nitzTime %s receivedTime %" PRId64, nitzTime.c_str(),
                nitzTimeReceived[slotId]);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->nitzTimeReceived(
                convertIntToRadioIndicationType(indicationType), nitzTime,
                nitzTimeReceived[slotId]);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nitzTimeReceivedInd: radioService[%d]->mRadioIndication == NULL", slotId);
        return -1;
    }

    return 0;
}

void convertRilSignalStrengthToHalV5(void *response, size_t responseLen,
        SignalStrength& signalStrength) {
    RIL_SignalStrength_v5 *rilSignalStrength = (RIL_SignalStrength_v5 *) response;
    int gsmSignalStrength;
    int cdmaDbm;
    int evdoDbm;

    gsmSignalStrength = rilSignalStrength->GW_SignalStrength.signalStrength & 0xFF;

#ifdef MODEM_TYPE_XMM6260
        if (gsmSignalStrength < 0 ||
                (gsmSignalStrength > 31 && rilSignalStrength->GW_SignalStrength.signalStrength != 99)) {
            gsmSignalStrength = rilSignalStrength->CDMA_SignalStrength.dbm;
        }
#else
        if (gsmSignalStrength < 0) {
            gsmSignalStrength = 99;
        } else if (gsmSignalStrength > 31 && gsmSignalStrength != 99) {
            gsmSignalStrength = 31;
        }
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm & 0xFF;
        if (cdmaDbm < 0) {
            cdmaDbm = 99;
        } else if (cdmaDbm > 31 && cdmaDbm != 99) {
            cdmaDbm = 31;
        }
#else
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm;
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm & 0xFF;
        if (evdoDbm < 0) {
            evdoDbm = 99;
        } else if (evdoDbm > 31 && evdoDbm != 99) {
            evdoDbm = 31;
        }
#else
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm;
#endif

    signalStrength.gw.signalStrength = gsmSignalStrength;
    signalStrength.gw.bitErrorRate = rilSignalStrength->GW_SignalStrength.bitErrorRate;
    signalStrength.cdma.dbm = cdmaDbm;
    signalStrength.cdma.ecio = rilSignalStrength->CDMA_SignalStrength.ecio;
    signalStrength.evdo.dbm = evdoDbm;
    signalStrength.evdo.ecio = rilSignalStrength->EVDO_SignalStrength.ecio;
    signalStrength.evdo.signalNoiseRatio =
            rilSignalStrength->EVDO_SignalStrength.signalNoiseRatio;
    signalStrength.lte.signalStrength = 99;
    signalStrength.lte.rsrp = INT_MAX;
    signalStrength.lte.rsrq = INT_MAX;
    signalStrength.lte.rssnr = INT_MAX;
    signalStrength.lte.cqi = INT_MAX;
    signalStrength.lte.timingAdvance = INT_MAX;
    signalStrength.tdScdma.rscp = INT_MAX;
}

void convertRilSignalStrengthToHalV6(void *response, size_t responseLen,
        SignalStrength& signalStrength) {
    RIL_SignalStrength_v6 *rilSignalStrength = (RIL_SignalStrength_v6 *) response;
    int gsmSignalStrength;
    int cdmaDbm;
    int evdoDbm;

    gsmSignalStrength = rilSignalStrength->GW_SignalStrength.signalStrength & 0xFF;

#ifdef MODEM_TYPE_XMM6260
        if (gsmSignalStrength < 0 ||
                (gsmSignalStrength > 31 && rilSignalStrength->GW_SignalStrength.signalStrength != 99)) {
            gsmSignalStrength = rilSignalStrength->CDMA_SignalStrength.dbm;
        }
#else
        if (gsmSignalStrength < 0) {
            gsmSignalStrength = 99;
        } else if (gsmSignalStrength > 31 && gsmSignalStrength != 99) {
            gsmSignalStrength = 31;
        }
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm & 0xFF;
        if (cdmaDbm < 0) {
            cdmaDbm = 99;
        } else if (cdmaDbm > 31 && cdmaDbm != 99) {
            cdmaDbm = 31;
        }
#else
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm;
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm & 0xFF;
        if (evdoDbm < 0) {
            evdoDbm = 99;
        } else if (evdoDbm > 31 && evdoDbm != 99) {
            evdoDbm = 31;
        }
#else
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm;
#endif

    // Fixup LTE for backwards compatibility
    // signalStrength: -1 -> 99
    if (rilSignalStrength->LTE_SignalStrength.signalStrength == -1) {
        rilSignalStrength->LTE_SignalStrength.signalStrength = 99;
    }
    // rsrp: -1 -> INT_MAX all other negative value to positive.
    // So remap here
    if (rilSignalStrength->LTE_SignalStrength.rsrp == -1) {
        rilSignalStrength->LTE_SignalStrength.rsrp = INT_MAX;
    } else if (rilSignalStrength->LTE_SignalStrength.rsrp < -1) {
        rilSignalStrength->LTE_SignalStrength.rsrp = -rilSignalStrength->LTE_SignalStrength.rsrp;
    }
    // rsrq: -1 -> INT_MAX
    if (rilSignalStrength->LTE_SignalStrength.rsrq == -1) {
        rilSignalStrength->LTE_SignalStrength.rsrq = INT_MAX;
    }
    // Not remapping rssnr is already using INT_MAX
    // cqi: -1 -> INT_MAX
    if (rilSignalStrength->LTE_SignalStrength.cqi == -1) {
        rilSignalStrength->LTE_SignalStrength.cqi = INT_MAX;
    }

    signalStrength.gw.signalStrength = gsmSignalStrength;
    signalStrength.gw.bitErrorRate = rilSignalStrength->GW_SignalStrength.bitErrorRate;
    signalStrength.cdma.dbm = cdmaDbm;
    signalStrength.cdma.ecio = rilSignalStrength->CDMA_SignalStrength.ecio;
    signalStrength.evdo.dbm = evdoDbm;
    signalStrength.evdo.ecio = rilSignalStrength->EVDO_SignalStrength.ecio;
    signalStrength.evdo.signalNoiseRatio =
            rilSignalStrength->EVDO_SignalStrength.signalNoiseRatio;
    signalStrength.lte.signalStrength = rilSignalStrength->LTE_SignalStrength.signalStrength;
    signalStrength.lte.rsrp = rilSignalStrength->LTE_SignalStrength.rsrp;
    signalStrength.lte.rsrq = rilSignalStrength->LTE_SignalStrength.rsrq;
    signalStrength.lte.rssnr = rilSignalStrength->LTE_SignalStrength.rssnr;
    signalStrength.lte.cqi = rilSignalStrength->LTE_SignalStrength.cqi;
    signalStrength.lte.timingAdvance = INT_MAX;
    signalStrength.tdScdma.rscp = INT_MAX;
}

void convertRilSignalStrengthToHalV8(void *response, size_t responseLen,
        SignalStrength& signalStrength) {
    RIL_SignalStrength_v8 *rilSignalStrength = (RIL_SignalStrength_v8 *) response;
    int gsmSignalStrength;
    int cdmaDbm;
    int evdoDbm;

    gsmSignalStrength = rilSignalStrength->GW_SignalStrength.signalStrength & 0xFF;

#ifdef MODEM_TYPE_XMM6260
        if (gsmSignalStrength < 0 ||
                (gsmSignalStrength > 31 && rilSignalStrength->GW_SignalStrength.signalStrength != 99)) {
            gsmSignalStrength = rilSignalStrength->CDMA_SignalStrength.dbm;
        }
#else
        if (gsmSignalStrength < 0) {
            gsmSignalStrength = 99;
        } else if (gsmSignalStrength > 31 && gsmSignalStrength != 99) {
            gsmSignalStrength = 31;
        }
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm & 0xFF;
        if (cdmaDbm < 0) {
            cdmaDbm = 99;
        } else if (cdmaDbm > 31 && cdmaDbm != 99) {
            cdmaDbm = 31;
        }
#else
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm;
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm & 0xFF;
        if (evdoDbm < 0) {
            evdoDbm = 99;
        } else if (evdoDbm > 31 && evdoDbm != 99) {
            evdoDbm = 31;
        }
#else
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm;
#endif

    // Fixup LTE for backwards compatibility
    // signalStrength: -1 -> 99
    if (rilSignalStrength->LTE_SignalStrength.signalStrength == -1) {
        rilSignalStrength->LTE_SignalStrength.signalStrength = 99;
    }
    // rsrp: -1 -> INT_MAX all other negative value to positive.
    // So remap here
    if (rilSignalStrength->LTE_SignalStrength.rsrp == -1) {
        rilSignalStrength->LTE_SignalStrength.rsrp = INT_MAX;
    } else if (rilSignalStrength->LTE_SignalStrength.rsrp < -1) {
        rilSignalStrength->LTE_SignalStrength.rsrp = -rilSignalStrength->LTE_SignalStrength.rsrp;
    }
    // rsrq: -1 -> INT_MAX
    if (rilSignalStrength->LTE_SignalStrength.rsrq == -1) {
        rilSignalStrength->LTE_SignalStrength.rsrq = INT_MAX;
    }
    // Not remapping rssnr is already using INT_MAX
    // cqi: -1 -> INT_MAX
    if (rilSignalStrength->LTE_SignalStrength.cqi == -1) {
        rilSignalStrength->LTE_SignalStrength.cqi = INT_MAX;
    }

    signalStrength.gw.signalStrength = gsmSignalStrength;
    signalStrength.gw.bitErrorRate = rilSignalStrength->GW_SignalStrength.bitErrorRate;
    signalStrength.cdma.dbm = cdmaDbm;
    signalStrength.cdma.ecio = rilSignalStrength->CDMA_SignalStrength.ecio;
    signalStrength.evdo.dbm = evdoDbm;
    signalStrength.evdo.ecio = rilSignalStrength->EVDO_SignalStrength.ecio;
    signalStrength.evdo.signalNoiseRatio =
            rilSignalStrength->EVDO_SignalStrength.signalNoiseRatio;
    signalStrength.lte.signalStrength = rilSignalStrength->LTE_SignalStrength.signalStrength;
    signalStrength.lte.rsrp = rilSignalStrength->LTE_SignalStrength.rsrp;
    signalStrength.lte.rsrq = rilSignalStrength->LTE_SignalStrength.rsrq;
    signalStrength.lte.rssnr = rilSignalStrength->LTE_SignalStrength.rssnr;
    signalStrength.lte.cqi = rilSignalStrength->LTE_SignalStrength.cqi;
    signalStrength.lte.timingAdvance = rilSignalStrength->LTE_SignalStrength.timingAdvance;
    signalStrength.tdScdma.rscp = INT_MAX;
}

void convertRilSignalStrengthToHalV10(void *response, size_t responseLen,
        SignalStrength& signalStrength) {
    RIL_SignalStrength_v10 *rilSignalStrength = (RIL_SignalStrength_v10 *) response;
    int gsmSignalStrength;
    int cdmaDbm;
    int evdoDbm;

    gsmSignalStrength = rilSignalStrength->GW_SignalStrength.signalStrength & 0xFF;

#ifdef MODEM_TYPE_XMM6260
        if (gsmSignalStrength < 0 ||
                (gsmSignalStrength > 31 && rilSignalStrength->GW_SignalStrength.signalStrength != 99)) {
            gsmSignalStrength = rilSignalStrength->CDMA_SignalStrength.dbm;
        }
#else
        if (gsmSignalStrength < 0) {
            gsmSignalStrength = 99;
        } else if (gsmSignalStrength > 31 && gsmSignalStrength != 99) {
            gsmSignalStrength = 31;
        }
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm & 0xFF;
        if (cdmaDbm < 0) {
            cdmaDbm = 99;
        } else if (cdmaDbm > 31 && cdmaDbm != 99) {
            cdmaDbm = 31;
        }
#else
        cdmaDbm = rilSignalStrength->CDMA_SignalStrength.dbm;
#endif

#if defined(MODEM_TYPE_XMM6262) || defined(SAMSUNG_NEXT_GEN_MODEM)
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm & 0xFF;
        if (evdoDbm < 0) {
            evdoDbm = 99;
        } else if (evdoDbm > 31 && evdoDbm != 99) {
            evdoDbm = 31;
        }
#else
        evdoDbm = rilSignalStrength->EVDO_SignalStrength.dbm;
#endif

    // Fixup LTE for backwards compatibility
    // signalStrength: -1 -> 99
    if (rilSignalStrength->LTE_SignalStrength.signalStrength == -1) {
        rilSignalStrength->LTE_SignalStrength.signalStrength = 99;
    }
    // rsrp: -1 -> INT_MAX all other negative value to positive.
    // So remap here
    if (rilSignalStrength->LTE_SignalStrength.rsrp == -1) {
        rilSignalStrength->LTE_SignalStrength.rsrp = INT_MAX;
    } else if (rilSignalStrength->LTE_SignalStrength.rsrp < -1) {
        rilSignalStrength->LTE_SignalStrength.rsrp = -rilSignalStrength->LTE_SignalStrength.rsrp;
    }
    // rsrq: -1 -> INT_MAX
    if (rilSignalStrength->LTE_SignalStrength.rsrq == -1) {
        rilSignalStrength->LTE_SignalStrength.rsrq = INT_MAX;
    }
    // Not remapping rssnr is already using INT_MAX
    // cqi: -1 -> INT_MAX
    if (rilSignalStrength->LTE_SignalStrength.cqi == -1) {
        rilSignalStrength->LTE_SignalStrength.cqi = INT_MAX;
    }

    signalStrength.gw.signalStrength = gsmSignalStrength;
    signalStrength.gw.bitErrorRate = rilSignalStrength->GW_SignalStrength.bitErrorRate;
    // RIL_SignalStrength_v10 not support gw.timingAdvance. Set to INT_MAX as
    // invalid value.
    signalStrength.gw.timingAdvance = INT_MAX;

    signalStrength.cdma.dbm = cdmaDbm;
    signalStrength.cdma.ecio = rilSignalStrength->CDMA_SignalStrength.ecio;
    signalStrength.evdo.dbm = evdoDbm;
    signalStrength.evdo.ecio = rilSignalStrength->EVDO_SignalStrength.ecio;
    signalStrength.evdo.signalNoiseRatio =
            rilSignalStrength->EVDO_SignalStrength.signalNoiseRatio;
    signalStrength.lte.signalStrength = rilSignalStrength->LTE_SignalStrength.signalStrength;
    signalStrength.lte.rsrp = rilSignalStrength->LTE_SignalStrength.rsrp;
    signalStrength.lte.rsrq = rilSignalStrength->LTE_SignalStrength.rsrq;
    signalStrength.lte.rssnr = rilSignalStrength->LTE_SignalStrength.rssnr;
    signalStrength.lte.cqi = rilSignalStrength->LTE_SignalStrength.cqi;
    signalStrength.lte.timingAdvance = rilSignalStrength->LTE_SignalStrength.timingAdvance;
    signalStrength.tdScdma.rscp = rilSignalStrength->TD_SCDMA_SignalStrength.rscp;
}

void convertRilSignalStrengthToHal(void *response, size_t responseLen,
        SignalStrength& signalStrength) {
    if (responseLen == sizeof(RIL_SignalStrength_v5)) {
        convertRilSignalStrengthToHalV5(response, responseLen, signalStrength);
    } else if (responseLen == sizeof(RIL_SignalStrength_v6)) {
        convertRilSignalStrengthToHalV6(response, responseLen, signalStrength);
    } else if (responseLen == sizeof(RIL_SignalStrength_v8)) {
        convertRilSignalStrengthToHalV8(response, responseLen, signalStrength);
    } else {
        convertRilSignalStrengthToHalV10(response, responseLen, signalStrength);
    }
}

void convertRilSignalStrengthToHal_1_6(int slotId, void *response, size_t responseLen,
        V1_6::SignalStrength& out) {
    SignalStrength legacy = {};
    convertRilSignalStrengthToHal(response, responseLen, legacy);

    out.gsm = legacy.gw;
    out.cdma = legacy.cdma;
    out.evdo = legacy.evdo;
    out.lte.base = legacy.lte;
    out.lte.cqiTableIndex = INT_MAX;

    // RIL v5-v10 has no distinct WCDMA measurements.  Do not manufacture
    // values from GSM RSSI; explicitly mark the newer fields as unavailable.
    out.wcdma.base.signalStrength = INT_MAX;
    out.wcdma.base.bitErrorRate = INT_MAX;
    out.wcdma.rscp = INT_MAX;
    out.wcdma.ecno = INT_MAX;
    out.tdscdma.signalStrength = INT_MAX;
    out.tdscdma.bitErrorRate = INT_MAX;
    out.tdscdma.rscp = legacy.tdScdma.rscp;

    out.nr.base.ssRsrp = INT_MAX;
    out.nr.base.ssRsrq = INT_MAX;
    out.nr.base.ssSinr = INT_MAX;
    out.nr.base.csiRsrp = INT_MAX;
    out.nr.base.csiRsrq = INT_MAX;
    out.nr.base.csiSinr = INT_MAX;
    out.nr.csiCqiTableIndex = INT_MAX;
    out.nr.csiCqiReport.resize(0);

#ifndef SAMSUNG_NEXT_GEN_MODEM
    // XMM6260/6262/6360 expose a legacy GW measurement structure whose
    // unused CDMA/EVDO/LTE members contain vendor filler. Android 16 treats
    // those values as real measurements and can choose LTE as the primary
    // signal even while the modem is registered on EDGE. Mark unsupported
    // radio families unavailable instead of forwarding fabricated data.
    out.cdma.dbm = INT_MAX;
    out.cdma.ecio = INT_MAX;
    out.evdo.dbm = INT_MAX;
    out.evdo.ecio = INT_MAX;
    out.evdo.signalNoiseRatio = INT_MAX;
    out.lte.base.signalStrength = INT_MAX;
    out.lte.base.rsrp = INT_MAX;
    out.lte.base.rsrq = INT_MAX;
    out.lte.base.rssnr = INT_MAX;
    out.lte.base.cqi = INT_MAX;
    out.lte.base.timingAdvance = INT_MAX;

    const int registeredRat = getCachedDataRadioTechnology(slotId);
    if (registeredRat == RADIO_TECH_UMTS || registeredRat == RADIO_TECH_HSDPA ||
            registeredRat == RADIO_TECH_HSUPA || registeredRat == RADIO_TECH_HSPA ||
            registeredRat == RADIO_TECH_HSPAP) {
        out.wcdma.base.signalStrength = legacy.gw.signalStrength;
        out.wcdma.base.bitErrorRate = legacy.gw.bitErrorRate;
        out.gsm.signalStrength = INT_MAX;
        out.gsm.bitErrorRate = INT_MAX;
        out.gsm.timingAdvance = INT_MAX;
    }
#endif
}

int radio::currentSignalStrengthInd(int slotId,
                                    int indicationType, int token, RIL_Errno e,
                                    void *response, size_t responseLen) {
    if (radioService[slotId] != NULL &&
            (radioService[slotId]->mRadioIndicationV1_6 != NULL ||
             radioService[slotId]->mRadioIndication != NULL)) {
        if (response == NULL || (responseLen != sizeof(RIL_SignalStrength_v10)
                && responseLen != sizeof(RIL_SignalStrength_v8)
                && responseLen != sizeof(RIL_SignalStrength_v6)
                && responseLen != sizeof(RIL_SignalStrength_v5))) {
            RLOGE("currentSignalStrengthInd: invalid response");
            return 0;
        }

#if VDBG
        RLOGD("currentSignalStrengthInd");
#endif
        Return<void> retStatus;
        if (radioService[slotId]->mRadioIndicationV1_6 != NULL) {
            V1_6::SignalStrength signalStrength = {};
            convertRilSignalStrengthToHal_1_6(slotId, response, responseLen, signalStrength);
            retStatus = radioService[slotId]->mRadioIndicationV1_6->currentSignalStrength_1_6(
                    convertIntToRadioIndicationType(indicationType), signalStrength);
        } else {
            SignalStrength signalStrength = {};
            convertRilSignalStrengthToHal(response, responseLen, signalStrength);
            retStatus = radioService[slotId]->mRadioIndication->currentSignalStrength(
                    convertIntToRadioIndicationType(indicationType), signalStrength);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("currentSignalStrengthInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

void convertRilDataCallToHal(RIL_Data_Call_Response_v6 *dcResponse,
        SetupDataCallResult& dcResult) {
    dcResult.status = (DataCallFailCause) dcResponse->status;
    dcResult.suggestedRetryTime = dcResponse->suggestedRetryTime;
    dcResult.cid = dcResponse->cid;
    dcResult.active = dcResponse->active;
    dcResult.type = convertCharPtrToHidlString(dcResponse->type);
    dcResult.ifname = convertCharPtrToHidlString(dcResponse->ifname);
    dcResult.addresses = convertCharPtrToHidlString(dcResponse->addresses);
    dcResult.dnses = convertCharPtrToHidlString(dcResponse->dnses);
#if defined(MODEM_TYPE_XMM6262) || defined(MODEM_TYPE_XMM6260)
    dcResult.gateways = convertCharPtrToHidlString(dcResponse->addresses);
#else
    dcResult.gateways = convertCharPtrToHidlString(dcResponse->gateways);
#endif
    dcResult.pcscf = hidl_string();
    dcResult.mtu = 0;
}

void convertRilDataCallToHal(RIL_Data_Call_Response_v9 *dcResponse,
        SetupDataCallResult& dcResult) {
    dcResult.status = (DataCallFailCause) dcResponse->status;
    dcResult.suggestedRetryTime = dcResponse->suggestedRetryTime;
    dcResult.cid = dcResponse->cid;
    dcResult.active = dcResponse->active;
    dcResult.type = convertCharPtrToHidlString(dcResponse->type);
    dcResult.ifname = convertCharPtrToHidlString(dcResponse->ifname);
    dcResult.addresses = convertCharPtrToHidlString(dcResponse->addresses);
    dcResult.dnses = convertCharPtrToHidlString(dcResponse->dnses);
    dcResult.gateways = convertCharPtrToHidlString(dcResponse->gateways);
    dcResult.pcscf = convertCharPtrToHidlString(dcResponse->pcscf);
    dcResult.mtu = 0;
}

void convertRilDataCallToHal(RIL_Data_Call_Response_v11 *dcResponse,
        SetupDataCallResult& dcResult) {
    dcResult.status = (DataCallFailCause) dcResponse->status;
    dcResult.suggestedRetryTime = dcResponse->suggestedRetryTime;
    dcResult.cid = dcResponse->cid;
    dcResult.active = dcResponse->active;
    dcResult.type = convertCharPtrToHidlString(dcResponse->type);
    dcResult.ifname = convertCharPtrToHidlString(dcResponse->ifname);
    dcResult.addresses = convertCharPtrToHidlString(dcResponse->addresses);
    dcResult.dnses = convertCharPtrToHidlString(dcResponse->dnses);
    dcResult.gateways = convertCharPtrToHidlString(dcResponse->gateways);
    dcResult.pcscf = convertCharPtrToHidlString(dcResponse->pcscf);
    dcResult.mtu = dcResponse->mtu;
}

void convertRilDataCallListToHal(void *response, size_t responseLen,
        hidl_vec<SetupDataCallResult>& dcResultList) {
    int num;

    if ((responseLen % sizeof(RIL_Data_Call_Response_v11)) == 0) {
        num = responseLen / sizeof(RIL_Data_Call_Response_v11);
        RIL_Data_Call_Response_v11 *dcResponse = (RIL_Data_Call_Response_v11 *) response;
        dcResultList.resize(num);
        for (int i = 0; i < num; i++) {
            convertRilDataCallToHal(&dcResponse[i], dcResultList[i]);
        }
    } else if ((responseLen % sizeof(RIL_Data_Call_Response_v9)) == 0) {
        num = responseLen / sizeof(RIL_Data_Call_Response_v9);
        RIL_Data_Call_Response_v9 *dcResponse = (RIL_Data_Call_Response_v9 *) response;
        dcResultList.resize(num);
        for (int i = 0; i < num; i++) {
            convertRilDataCallToHal(&dcResponse[i], dcResultList[i]);
        }
    } else if ((responseLen % sizeof(RIL_Data_Call_Response_v6)) == 0) {
        num = responseLen / sizeof(RIL_Data_Call_Response_v6);
        RIL_Data_Call_Response_v6 *dcResponse = (RIL_Data_Call_Response_v6 *) response;
        dcResultList.resize(num);
        for (int i = 0; i < num; i++) {
            convertRilDataCallToHal(&dcResponse[i], dcResultList[i]);
        }
    }
}

static hidl_vec<hidl_string> splitSpaceSeparated(const hidl_string& value) {
    std::vector<hidl_string> result;
    std::istringstream stream(value.c_str());
    std::string item;
    while (stream >> item) result.emplace_back(item);
    return result;
}

static V1_4::PdpProtocolType convertPdpProtocolType(const hidl_string& value) {
    if (value == "IPV4V6") return V1_4::PdpProtocolType::IPV4V6;
    if (value == "IPV6") return V1_4::PdpProtocolType::IPV6;
    if (value == "IP") return V1_4::PdpProtocolType::IP;
    if (value == "PPP") return V1_4::PdpProtocolType::PPP;
    if (value == "NON_IP") return V1_4::PdpProtocolType::NON_IP;
    if (value == "UNSTRUCTURED") return V1_4::PdpProtocolType::UNSTRUCTURED;
    return V1_4::PdpProtocolType::UNKNOWN;
}

static void convertLegacyDataCallResultToHal_1_6(const SetupDataCallResult& in,
        V1_6::SetupDataCallResult& out) {
    out.cause = static_cast<V1_6::DataCallFailCause>(in.status);
    out.suggestedRetryTime = in.suggestedRetryTime;
    out.cid = in.cid;
    out.active = static_cast<V1_4::DataConnActiveStatus>(in.active);
    out.type = convertPdpProtocolType(in.type);
    out.ifname = in.ifname;

    std::vector<V1_5::LinkAddress> addresses;
    std::istringstream addressStream(in.addresses.c_str());
    std::string address;
    while (addressStream >> address) {
        V1_5::LinkAddress linkAddress = {};
        linkAddress.address = address;
        linkAddress.properties = 0;
        linkAddress.deprecationTime = INT64_MAX;
        linkAddress.expirationTime = INT64_MAX;
        addresses.push_back(linkAddress);
    }
    out.addresses = addresses;
    out.dnses = splitSpaceSeparated(in.dnses);
    out.gateways = splitSpaceSeparated(in.gateways);
    out.pcscf = splitSpaceSeparated(in.pcscf);
    out.mtuV4 = in.mtu;
    out.mtuV6 = in.mtu;
    out.qosSessions.resize(0);
    out.pduSessionId = 0;
    out.trafficDescriptors.resize(0);
}

void convertRilDataCallToHal_1_6(void *response, size_t responseLen,
        V1_6::SetupDataCallResult& out) {
    SetupDataCallResult legacy = {};
    if (responseLen == sizeof(RIL_Data_Call_Response_v11)) {
        convertRilDataCallToHal(static_cast<RIL_Data_Call_Response_v11 *>(response), legacy);
    } else if (responseLen == sizeof(RIL_Data_Call_Response_v9)) {
        convertRilDataCallToHal(static_cast<RIL_Data_Call_Response_v9 *>(response), legacy);
    } else if (responseLen == sizeof(RIL_Data_Call_Response_v6)) {
        convertRilDataCallToHal(static_cast<RIL_Data_Call_Response_v6 *>(response), legacy);
    }
    convertLegacyDataCallResultToHal_1_6(legacy, out);
}

void convertRilDataCallListToHal_1_6(void *response, size_t responseLen,
        hidl_vec<V1_6::SetupDataCallResult>& out) {
    hidl_vec<SetupDataCallResult> legacy;
    convertRilDataCallListToHal(response, responseLen, legacy);
    out.resize(legacy.size());
    for (size_t i = 0; i < legacy.size(); ++i) {
        convertLegacyDataCallResultToHal_1_6(legacy[i], out[i]);
    }
}

int radio::dataCallListChangedInd(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (radioService[slotId] != NULL &&
            (radioService[slotId]->mRadioIndicationV1_6 != NULL ||
             radioService[slotId]->mRadioIndication != NULL)) {
        if ((response == NULL && responseLen != 0)
                || (responseLen % sizeof(RIL_Data_Call_Response_v11) != 0
                && responseLen % sizeof(RIL_Data_Call_Response_v9) != 0
                && responseLen % sizeof(RIL_Data_Call_Response_v6) != 0)) {
            RLOGE("dataCallListChangedInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("dataCallListChangedInd");
#endif
        Return<void> retStatus;
        if (radioService[slotId]->mRadioIndicationV1_6 != NULL) {
            hidl_vec<V1_6::SetupDataCallResult> dcList;
            convertRilDataCallListToHal_1_6(response, responseLen, dcList);
            retStatus = radioService[slotId]->mRadioIndicationV1_6->dataCallListChanged_1_6(
                    convertIntToRadioIndicationType(indicationType), dcList);
        } else {
            hidl_vec<SetupDataCallResult> dcList;
            convertRilDataCallListToHal(response, responseLen, dcList);
            retStatus = radioService[slotId]->mRadioIndication->dataCallListChanged(
                    convertIntToRadioIndicationType(indicationType), dcList);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("dataCallListChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::suppSvcNotifyInd(int slotId, int indicationType,
                            int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_SuppSvcNotification)) {
            RLOGE("suppSvcNotifyInd: invalid response");
            return 0;
        }

        SuppSvcNotification suppSvc = {};
        RIL_SuppSvcNotification *ssn = (RIL_SuppSvcNotification *) response;
        suppSvc.isMT = ssn->notificationType;
        suppSvc.code = ssn->code;
        suppSvc.index = ssn->index;
        suppSvc.type = ssn->type;
        suppSvc.number = convertCharPtrToHidlString(ssn->number);

#if VDBG
        RLOGD("suppSvcNotifyInd: isMT %d code %d index %d type %d",
                suppSvc.isMT, suppSvc.code, suppSvc.index, suppSvc.type);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->suppSvcNotify(
                convertIntToRadioIndicationType(indicationType), suppSvc);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("suppSvcNotifyInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkSessionEndInd(int slotId, int indicationType,
                            int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("stkSessionEndInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkSessionEnd(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkSessionEndInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkProactiveCommandInd(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("stkProactiveCommandInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("stkProactiveCommandInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkProactiveCommand(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkProactiveCommandInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkEventNotifyInd(int slotId, int indicationType,
                             int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("stkEventNotifyInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("stkEventNotifyInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkEventNotify(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkEventNotifyInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkCallSetupInd(int slotId, int indicationType,
                           int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("stkCallSetupInd: invalid response");
            return 0;
        }
        int32_t timeout = ((int32_t *) response)[0];
#if VDBG
        RLOGD("stkCallSetupInd: timeout %d", timeout);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkCallSetup(
                convertIntToRadioIndicationType(indicationType), timeout);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkCallSetupInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simSmsStorageFullInd(int slotId,
                                int indicationType, int token, RIL_Errno e, void *response,
                                size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("simSmsStorageFullInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->simSmsStorageFull(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("simSmsStorageFullInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simRefreshInd(int slotId, int indicationType,
                         int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_SimRefreshResponse_v7)) {
            RLOGE("simRefreshInd: invalid response");
            return 0;
        }

        SimRefreshResult refreshResult = {};
        RIL_SimRefreshResponse_v7 *simRefreshResponse = ((RIL_SimRefreshResponse_v7 *) response);
        refreshResult.type =
                (V1_0::SimRefreshType) simRefreshResponse->result;
        refreshResult.efId = simRefreshResponse->ef_id;
        refreshResult.aid = convertCharPtrToHidlString(simRefreshResponse->aid);

#if VDBG
        RLOGD("simRefreshInd: type %d efId %d", refreshResult.type, refreshResult.efId);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->simRefresh(
                convertIntToRadioIndicationType(indicationType), refreshResult);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("simRefreshInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

void convertRilCdmaSignalInfoRecordToHal(RIL_CDMA_SignalInfoRecord *signalInfoRecord,
        CdmaSignalInfoRecord& record) {
    record.isPresent = signalInfoRecord->isPresent;
    record.signalType = signalInfoRecord->signalType;
    record.alertPitch = signalInfoRecord->alertPitch;
    record.signal = signalInfoRecord->signal;
}

int radio::callRingInd(int slotId, int indicationType,
                       int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        bool isGsm;
        CdmaSignalInfoRecord record = {};
        if (response == NULL || responseLen == 0) {
            isGsm = true;
        } else {
            isGsm = false;
            if (responseLen != sizeof (RIL_CDMA_SignalInfoRecord)) {
                RLOGE("callRingInd: invalid response");
                return 0;
            }
            convertRilCdmaSignalInfoRecordToHal((RIL_CDMA_SignalInfoRecord *) response, record);
        }

#if VDBG
        RLOGD("callRingInd: isGsm %d", isGsm);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->callRing(
                convertIntToRadioIndicationType(indicationType), isGsm, record);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("callRingInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simStatusChangedInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("simStatusChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->simStatusChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("simStatusChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaNewSmsInd(int slotId, int indicationType,
                         int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_CDMA_SMS_Message)) {
            RLOGE("cdmaNewSmsInd: invalid response");
            return 0;
        }

        CdmaSmsMessage msg = {};
        RIL_CDMA_SMS_Message *rilMsg = (RIL_CDMA_SMS_Message *) response;
        msg.teleserviceId = rilMsg->uTeleserviceID;
        msg.isServicePresent = rilMsg->bIsServicePresent;
        msg.serviceCategory = rilMsg->uServicecategory;
        msg.address.digitMode =
                (V1_0::CdmaSmsDigitMode) rilMsg->sAddress.digit_mode;
        msg.address.numberMode =
                (V1_0::CdmaSmsNumberMode) rilMsg->sAddress.number_mode;
        msg.address.numberType =
                (V1_0::CdmaSmsNumberType) rilMsg->sAddress.number_type;
        msg.address.numberPlan =
                (V1_0::CdmaSmsNumberPlan) rilMsg->sAddress.number_plan;

        int digitLimit = MIN((rilMsg->sAddress.number_of_digits), RIL_CDMA_SMS_ADDRESS_MAX);
        msg.address.digits.setToExternal(rilMsg->sAddress.digits, digitLimit);

        msg.subAddress.subaddressType = (V1_0::CdmaSmsSubaddressType)
                rilMsg->sSubAddress.subaddressType;
        msg.subAddress.odd = rilMsg->sSubAddress.odd;

        digitLimit= MIN((rilMsg->sSubAddress.number_of_digits), RIL_CDMA_SMS_SUBADDRESS_MAX);
        msg.subAddress.digits.setToExternal(rilMsg->sSubAddress.digits, digitLimit);

        digitLimit = MIN((rilMsg->uBearerDataLen), RIL_CDMA_SMS_BEARER_DATA_MAX);
        msg.bearerData.setToExternal(rilMsg->aBearerData, digitLimit);

#if VDBG
        RLOGD("cdmaNewSmsInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaNewSms(
                convertIntToRadioIndicationType(indicationType), msg);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaNewSmsInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::newBroadcastSmsInd(int slotId,
                              int indicationType, int token, RIL_Errno e, void *response,
                              size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("newBroadcastSmsInd: invalid response");
            return 0;
        }

        hidl_vec<uint8_t> data;
        data.setToExternal((uint8_t *) response, responseLen);
#if VDBG
        RLOGD("newBroadcastSmsInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newBroadcastSms(
                convertIntToRadioIndicationType(indicationType), data);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("newBroadcastSmsInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaRuimSmsStorageFullInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("cdmaRuimSmsStorageFullInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaRuimSmsStorageFull(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaRuimSmsStorageFullInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::restrictedStateChangedInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("restrictedStateChangedInd: invalid response");
            return 0;
        }
        int32_t state = ((int32_t *) response)[0];
#if VDBG
        RLOGD("restrictedStateChangedInd: state %d", state);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->restrictedStateChanged(
                convertIntToRadioIndicationType(indicationType), (PhoneRestrictedState) state);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("restrictedStateChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::enterEmergencyCallbackModeInd(int slotId,
                                         int indicationType, int token, RIL_Errno e, void *response,
                                         size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("enterEmergencyCallbackModeInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->enterEmergencyCallbackMode(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("enterEmergencyCallbackModeInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::cdmaCallWaitingInd(int slotId,
                              int indicationType, int token, RIL_Errno e, void *response,
                              size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_CDMA_CallWaiting_v6)) {
            RLOGE("cdmaCallWaitingInd: invalid response");
            return 0;
        }

        CdmaCallWaiting callWaitingRecord = {};
        RIL_CDMA_CallWaiting_v6 *callWaitingRil = ((RIL_CDMA_CallWaiting_v6 *) response);
        callWaitingRecord.number = convertCharPtrToHidlString(callWaitingRil->number);
        callWaitingRecord.numberPresentation =
                (CdmaCallWaitingNumberPresentation) callWaitingRil->numberPresentation;
        callWaitingRecord.name = convertCharPtrToHidlString(callWaitingRil->name);
        convertRilCdmaSignalInfoRecordToHal(&callWaitingRil->signalInfoRecord,
                callWaitingRecord.signalInfoRecord);
        callWaitingRecord.numberType = (CdmaCallWaitingNumberType) callWaitingRil->number_type;
        callWaitingRecord.numberPlan = (CdmaCallWaitingNumberPlan) callWaitingRil->number_plan;

#if VDBG
        RLOGD("cdmaCallWaitingInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaCallWaiting(
                convertIntToRadioIndicationType(indicationType), callWaitingRecord);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaCallWaitingInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaOtaProvisionStatusInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("cdmaOtaProvisionStatusInd: invalid response");
            return 0;
        }
        int32_t status = ((int32_t *) response)[0];
#if VDBG
        RLOGD("cdmaOtaProvisionStatusInd: status %d", status);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaOtaProvisionStatus(
                convertIntToRadioIndicationType(indicationType), (CdmaOtaProvisionStatus) status);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaOtaProvisionStatusInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::cdmaInfoRecInd(int slotId,
                          int indicationType, int token, RIL_Errno e, void *response,
                          size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_CDMA_InformationRecords)) {
            RLOGE("cdmaInfoRecInd: invalid response");
            return 0;
        }

        CdmaInformationRecords records = {};
        RIL_CDMA_InformationRecords *recordsRil = (RIL_CDMA_InformationRecords *) response;

        char* string8 = NULL;
        int num = MIN(recordsRil->numberOfInfoRecs, RIL_CDMA_MAX_NUMBER_OF_INFO_RECS);
        if (recordsRil->numberOfInfoRecs > RIL_CDMA_MAX_NUMBER_OF_INFO_RECS) {
            RLOGE("cdmaInfoRecInd: received %d recs which is more than %d, dropping "
                    "additional ones", recordsRil->numberOfInfoRecs,
                    RIL_CDMA_MAX_NUMBER_OF_INFO_RECS);
        }
        records.infoRec.resize(num);
        for (int i = 0 ; i < num ; i++) {
            CdmaInformationRecord *record = &records.infoRec[i];
            RIL_CDMA_InformationRecord *infoRec = &recordsRil->infoRec[i];
            record->name = (CdmaInfoRecName) infoRec->name;
            // All vectors should be size 0 except one which will be size 1. Set everything to
            // size 0 initially.
            record->display.resize(0);
            record->number.resize(0);
            record->signal.resize(0);
            record->redir.resize(0);
            record->lineCtrl.resize(0);
            record->clir.resize(0);
            record->audioCtrl.resize(0);
            switch (infoRec->name) {
                case RIL_CDMA_DISPLAY_INFO_REC:
                case RIL_CDMA_EXTENDED_DISPLAY_INFO_REC: {
                    if (infoRec->rec.display.alpha_len > CDMA_ALPHA_INFO_BUFFER_LENGTH) {
                        RLOGE("cdmaInfoRecInd: invalid display info response length %d "
                                "expected not more than %d", (int) infoRec->rec.display.alpha_len,
                                CDMA_ALPHA_INFO_BUFFER_LENGTH);
                        return 0;
                    }
                    string8 = (char*) malloc((infoRec->rec.display.alpha_len + 1) * sizeof(char));
                    if (string8 == NULL) {
                        RLOGE("cdmaInfoRecInd: Memory allocation failed for "
                                "responseCdmaInformationRecords");
                        return 0;
                    }
                    memcpy(string8, infoRec->rec.display.alpha_buf, infoRec->rec.display.alpha_len);
                    string8[(int)infoRec->rec.display.alpha_len] = '\0';

                    record->display.resize(1);
                    record->display[0].alphaBuf = string8;
                    free(string8);
                    string8 = NULL;
                    break;
                }

                case RIL_CDMA_CALLED_PARTY_NUMBER_INFO_REC:
                case RIL_CDMA_CALLING_PARTY_NUMBER_INFO_REC:
                case RIL_CDMA_CONNECTED_NUMBER_INFO_REC: {
                    if (infoRec->rec.number.len > CDMA_NUMBER_INFO_BUFFER_LENGTH) {
                        RLOGE("cdmaInfoRecInd: invalid display info response length %d "
                                "expected not more than %d", (int) infoRec->rec.number.len,
                                CDMA_NUMBER_INFO_BUFFER_LENGTH);
                        return 0;
                    }
                    string8 = (char*) malloc((infoRec->rec.number.len + 1) * sizeof(char));
                    if (string8 == NULL) {
                        RLOGE("cdmaInfoRecInd: Memory allocation failed for "
                                "responseCdmaInformationRecords");
                        return 0;
                    }
                    memcpy(string8, infoRec->rec.number.buf, infoRec->rec.number.len);
                    string8[(int)infoRec->rec.number.len] = '\0';

                    record->number.resize(1);
                    record->number[0].number = string8;
                    free(string8);
                    string8 = NULL;
                    record->number[0].numberType = infoRec->rec.number.number_type;
                    record->number[0].numberPlan = infoRec->rec.number.number_plan;
                    record->number[0].pi = infoRec->rec.number.pi;
                    record->number[0].si = infoRec->rec.number.si;
                    break;
                }

                case RIL_CDMA_SIGNAL_INFO_REC: {
                    record->signal.resize(1);
                    record->signal[0].isPresent = infoRec->rec.signal.isPresent;
                    record->signal[0].signalType = infoRec->rec.signal.signalType;
                    record->signal[0].alertPitch = infoRec->rec.signal.alertPitch;
                    record->signal[0].signal = infoRec->rec.signal.signal;

                    /* Drop the response to workaround the "ring of death" bug */
                    if (infoRec->rec.signal.isPresent
                            /* IS95_CONST_IR_SIGNAL_IS54B */
                            && infoRec->rec.signal.signalType == 2
                            /* IS95_CONST_IR_ALERT_MED */
                            && infoRec->rec.signal.alertPitch == 0
                            /* IS95_CONST_IR_SIG_IS54B_L */
                            && infoRec->rec.signal.signal == 1) {
                        return 0;
                    }

                    break;
                }

                case RIL_CDMA_REDIRECTING_NUMBER_INFO_REC: {
                    if (infoRec->rec.redir.redirectingNumber.len >
                                                  CDMA_NUMBER_INFO_BUFFER_LENGTH) {
                        RLOGE("cdmaInfoRecInd: invalid display info response length %d "
                                "expected not more than %d\n",
                                (int)infoRec->rec.redir.redirectingNumber.len,
                                CDMA_NUMBER_INFO_BUFFER_LENGTH);
                        return 0;
                    }
                    string8 = (char*) malloc((infoRec->rec.redir.redirectingNumber.len + 1) *
                            sizeof(char));
                    if (string8 == NULL) {
                        RLOGE("cdmaInfoRecInd: Memory allocation failed for "
                                "responseCdmaInformationRecords");
                        return 0;
                    }
                    memcpy(string8, infoRec->rec.redir.redirectingNumber.buf,
                            infoRec->rec.redir.redirectingNumber.len);
                    string8[(int)infoRec->rec.redir.redirectingNumber.len] = '\0';

                    record->redir.resize(1);
                    record->redir[0].redirectingNumber.number = string8;
                    free(string8);
                    string8 = NULL;
                    record->redir[0].redirectingNumber.numberType =
                            infoRec->rec.redir.redirectingNumber.number_type;
                    record->redir[0].redirectingNumber.numberPlan =
                            infoRec->rec.redir.redirectingNumber.number_plan;
                    record->redir[0].redirectingNumber.pi = infoRec->rec.redir.redirectingNumber.pi;
                    record->redir[0].redirectingNumber.si = infoRec->rec.redir.redirectingNumber.si;
                    record->redir[0].redirectingReason =
                            (CdmaRedirectingReason) infoRec->rec.redir.redirectingReason;
                    break;
                }

                case RIL_CDMA_LINE_CONTROL_INFO_REC: {
                    record->lineCtrl.resize(1);
                    record->lineCtrl[0].lineCtrlPolarityIncluded =
                            infoRec->rec.lineCtrl.lineCtrlPolarityIncluded;
                    record->lineCtrl[0].lineCtrlToggle = infoRec->rec.lineCtrl.lineCtrlToggle;
                    record->lineCtrl[0].lineCtrlReverse = infoRec->rec.lineCtrl.lineCtrlReverse;
                    record->lineCtrl[0].lineCtrlPowerDenial =
                            infoRec->rec.lineCtrl.lineCtrlPowerDenial;
                    break;
                }

                case RIL_CDMA_T53_CLIR_INFO_REC: {
                    record->clir.resize(1);
                    record->clir[0].cause = infoRec->rec.clir.cause;
                    break;
                }

                case RIL_CDMA_T53_AUDIO_CONTROL_INFO_REC: {
                    record->audioCtrl.resize(1);
                    record->audioCtrl[0].upLink = infoRec->rec.audioCtrl.upLink;
                    record->audioCtrl[0].downLink = infoRec->rec.audioCtrl.downLink;
                    break;
                }

                case RIL_CDMA_T53_RELEASE_INFO_REC:
                    RLOGE("cdmaInfoRecInd: RIL_CDMA_T53_RELEASE_INFO_REC: INVALID");
                    return 0;

                default:
                    RLOGE("cdmaInfoRecInd: Incorrect name value");
                    return 0;
            }
        }

#if VDBG
        RLOGD("cdmaInfoRecInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaInfoRec(
                convertIntToRadioIndicationType(indicationType), records);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaInfoRecInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::indicateRingbackToneInd(int slotId,
                                   int indicationType, int token, RIL_Errno e, void *response,
                                   size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("indicateRingbackToneInd: invalid response");
            return 0;
        }
        bool start = ((int32_t *) response)[0];
#if VDBG
        RLOGD("indicateRingbackToneInd: start %d", start);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->indicateRingbackTone(
                convertIntToRadioIndicationType(indicationType), start);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("indicateRingbackToneInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::resendIncallMuteInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("resendIncallMuteInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->resendIncallMute(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("resendIncallMuteInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaSubscriptionSourceChangedInd(int slotId,
                                            int indicationType, int token, RIL_Errno e,
                                            void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("cdmaSubscriptionSourceChangedInd: invalid response");
            return 0;
        }
        int32_t cdmaSource = ((int32_t *) response)[0];
#if VDBG
        RLOGD("cdmaSubscriptionSourceChangedInd: cdmaSource %d", cdmaSource);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->
                cdmaSubscriptionSourceChanged(convertIntToRadioIndicationType(indicationType),
                (CdmaSubscriptionSource) cdmaSource);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaSubscriptionSourceChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::cdmaPrlChangedInd(int slotId,
                             int indicationType, int token, RIL_Errno e, void *response,
                             size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("cdmaPrlChangedInd: invalid response");
            return 0;
        }
        int32_t version = ((int32_t *) response)[0];
#if VDBG
        RLOGD("cdmaPrlChangedInd: version %d", version);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaPrlChanged(
                convertIntToRadioIndicationType(indicationType), version);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaPrlChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::exitEmergencyCallbackModeInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("exitEmergencyCallbackModeInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->exitEmergencyCallbackMode(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("exitEmergencyCallbackModeInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::rilConnectedInd(int slotId,
                           int indicationType, int token, RIL_Errno e, void *response,
                           size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        RLOGD("rilConnectedInd");
        Return<void> retStatus = radioService[slotId]->mRadioIndication->rilConnected(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("rilConnectedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::voiceRadioTechChangedInd(int slotId,
                                    int indicationType, int token, RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("voiceRadioTechChangedInd: invalid response");
            return 0;
        }
        int32_t rat = ((int32_t *) response)[0];
#if VDBG
        RLOGD("voiceRadioTechChangedInd: rat %d", rat);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->voiceRadioTechChanged(
                convertIntToRadioIndicationType(indicationType), (RadioTechnology) rat);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("voiceRadioTechChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

void convertRilCellInfoListToHal(void *response, size_t responseLen, hidl_vec<CellInfo>& records) {
    int num = responseLen / sizeof(RIL_CellInfo_v12);
    records.resize(num);

    RIL_CellInfo_v12 *rillCellInfo = (RIL_CellInfo_v12 *) response;
    for (int i = 0; i < num; i++) {
        records[i].cellInfoType = (CellInfoType) rillCellInfo->cellInfoType;
        records[i].registered = rillCellInfo->registered;
        records[i].timeStampType = (TimeStampType) rillCellInfo->timeStampType;
        records[i].timeStamp = rillCellInfo->timeStamp;
        // All vectors should be size 0 except one which will be size 1. Set everything to
        // size 0 initially.
        records[i].gsm.resize(0);
        records[i].wcdma.resize(0);
        records[i].cdma.resize(0);
        records[i].lte.resize(0);
        records[i].tdscdma.resize(0);
        switch(rillCellInfo->cellInfoType) {
            case RIL_CELL_INFO_TYPE_GSM: {
                records[i].gsm.resize(1);
                CellInfoGsm *cellInfoGsm = &records[i].gsm[0];
                cellInfoGsm->cellIdentityGsm.mcc =
                        std::to_string(rillCellInfo->CellInfo.gsm.cellIdentityGsm.mcc);
                cellInfoGsm->cellIdentityGsm.mnc =
                        ril::util::mnc::decode(rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                cellInfoGsm->cellIdentityGsm.lac =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.lac;
                cellInfoGsm->cellIdentityGsm.cid =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.cid;
                cellInfoGsm->cellIdentityGsm.arfcn =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.arfcn;
                cellInfoGsm->cellIdentityGsm.bsic =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.bsic;
                cellInfoGsm->signalStrengthGsm.signalStrength =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.signalStrength;
                cellInfoGsm->signalStrengthGsm.bitErrorRate =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.bitErrorRate;
                cellInfoGsm->signalStrengthGsm.timingAdvance =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.timingAdvance;
                break;
            }

            case RIL_CELL_INFO_TYPE_WCDMA: {
                records[i].wcdma.resize(1);
                CellInfoWcdma *cellInfoWcdma = &records[i].wcdma[0];
                cellInfoWcdma->cellIdentityWcdma.mcc =
                        std::to_string(rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mcc);
                cellInfoWcdma->cellIdentityWcdma.mnc =
                        ril::util::mnc::decode(rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                cellInfoWcdma->cellIdentityWcdma.lac =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.lac;
                cellInfoWcdma->cellIdentityWcdma.cid =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.cid;
                cellInfoWcdma->cellIdentityWcdma.psc =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.psc;
                cellInfoWcdma->cellIdentityWcdma.uarfcn =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.uarfcn;
                cellInfoWcdma->signalStrengthWcdma.signalStrength =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.signalStrength;
                cellInfoWcdma->signalStrengthWcdma.bitErrorRate =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.bitErrorRate;
                break;
            }

            case RIL_CELL_INFO_TYPE_CDMA: {
                records[i].cdma.resize(1);
                CellInfoCdma *cellInfoCdma = &records[i].cdma[0];
                cellInfoCdma->cellIdentityCdma.networkId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.networkId;
                cellInfoCdma->cellIdentityCdma.systemId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.systemId;
                cellInfoCdma->cellIdentityCdma.baseStationId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.basestationId;
                cellInfoCdma->cellIdentityCdma.longitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.longitude;
                cellInfoCdma->cellIdentityCdma.latitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.latitude;
                cellInfoCdma->signalStrengthCdma.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.dbm;
                cellInfoCdma->signalStrengthCdma.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.ecio;
                cellInfoCdma->signalStrengthEvdo.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.dbm;
                cellInfoCdma->signalStrengthEvdo.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.ecio;
                cellInfoCdma->signalStrengthEvdo.signalNoiseRatio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.signalNoiseRatio;
                break;
            }

            case RIL_CELL_INFO_TYPE_LTE: {
                records[i].lte.resize(1);
                CellInfoLte *cellInfoLte = &records[i].lte[0];
                cellInfoLte->cellIdentityLte.mcc =
                        std::to_string(rillCellInfo->CellInfo.lte.cellIdentityLte.mcc);
                cellInfoLte->cellIdentityLte.mnc =
                        ril::util::mnc::decode(rillCellInfo->CellInfo.lte.cellIdentityLte.mnc);
                cellInfoLte->cellIdentityLte.ci =
                        rillCellInfo->CellInfo.lte.cellIdentityLte.ci;
                cellInfoLte->cellIdentityLte.pci =
                        rillCellInfo->CellInfo.lte.cellIdentityLte.pci;
                cellInfoLte->cellIdentityLte.tac =
                        rillCellInfo->CellInfo.lte.cellIdentityLte.tac;
                cellInfoLte->cellIdentityLte.earfcn =
                        rillCellInfo->CellInfo.lte.cellIdentityLte.earfcn;
                cellInfoLte->signalStrengthLte.signalStrength =
                        rillCellInfo->CellInfo.lte.signalStrengthLte.signalStrength;
                cellInfoLte->signalStrengthLte.rsrp =
                        rillCellInfo->CellInfo.lte.signalStrengthLte.rsrp;
                cellInfoLte->signalStrengthLte.rsrq =
                        rillCellInfo->CellInfo.lte.signalStrengthLte.rsrq;
                cellInfoLte->signalStrengthLte.rssnr =
                        rillCellInfo->CellInfo.lte.signalStrengthLte.rssnr;
                cellInfoLte->signalStrengthLte.cqi =
                        rillCellInfo->CellInfo.lte.signalStrengthLte.cqi;
                cellInfoLte->signalStrengthLte.timingAdvance =
                        rillCellInfo->CellInfo.lte.signalStrengthLte.timingAdvance;
                break;
            }

            case RIL_CELL_INFO_TYPE_TD_SCDMA: {
                records[i].tdscdma.resize(1);
                CellInfoTdscdma *cellInfoTdscdma = &records[i].tdscdma[0];
                cellInfoTdscdma->cellIdentityTdscdma.mcc =
                        std::to_string(rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mcc);
                cellInfoTdscdma->cellIdentityTdscdma.mnc =
                        ril::util::mnc::decode(
                                rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                cellInfoTdscdma->cellIdentityTdscdma.lac =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.lac;
                cellInfoTdscdma->cellIdentityTdscdma.cid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cid;
                cellInfoTdscdma->cellIdentityTdscdma.cpid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cpid;
                cellInfoTdscdma->signalStrengthTdscdma.rscp =
                        rillCellInfo->CellInfo.tdscdma.signalStrengthTdscdma.rscp;
                break;
            }
            default: {
                break;
            }
        }
        rillCellInfo += 1;
    }
}

static hidl_string legacyMccToString(int mcc) {
    if (mcc < 0 || mcc > 999) return {};
    char value[4];
    snprintf(value, sizeof(value), "%03d", mcc);
    return value;
}

void convertRilCellInfoListToHal_1_6(int slotId, void *response, size_t responseLen,
        hidl_vec<V1_6::CellInfo>& records) {
    int num = responseLen / sizeof(RIL_CellInfo_v12);
    records.resize(num);
    RIL_CellInfo_v12 *rilInfo = static_cast<RIL_CellInfo_v12 *>(response);
    const std::string operatorNumeric = getCachedOperatorNumeric(slotId);

    for (int i = 0; i < num; ++i, ++rilInfo) {
        records[i].registered = rilInfo->registered;
        records[i].connectionStatus = rilInfo->registered
                ? V1_2::CellConnectionStatus::PRIMARY_SERVING
                : V1_2::CellConnectionStatus::NONE;

        switch (rilInfo->cellInfoType) {
            case RIL_CELL_INFO_TYPE_GSM: {
                V1_5::CellInfoGsm gsm = {};
                auto& identity = gsm.cellIdentityGsm.base.base;
                identity.mcc = legacyMccToString(rilInfo->CellInfo.gsm.cellIdentityGsm.mcc);
                identity.mnc = ril::util::mnc::decode(
                        rilInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                if (rilInfo->registered) {
                    fillMissingPlmn(identity.mcc, identity.mnc, operatorNumeric);
                }
                identity.lac = rilInfo->CellInfo.gsm.cellIdentityGsm.lac;
                identity.cid = rilInfo->CellInfo.gsm.cellIdentityGsm.cid;
                identity.arfcn = rilInfo->CellInfo.gsm.cellIdentityGsm.arfcn;
                identity.bsic = rilInfo->CellInfo.gsm.cellIdentityGsm.bsic;
                gsm.signalStrengthGsm.signalStrength =
                        rilInfo->CellInfo.gsm.signalStrengthGsm.signalStrength;
                gsm.signalStrengthGsm.bitErrorRate =
                        rilInfo->CellInfo.gsm.signalStrengthGsm.bitErrorRate;
                gsm.signalStrengthGsm.timingAdvance =
                        rilInfo->CellInfo.gsm.signalStrengthGsm.timingAdvance;
                records[i].ratSpecificInfo.gsm(gsm);
                break;
            }
            case RIL_CELL_INFO_TYPE_WCDMA: {
                V1_5::CellInfoWcdma wcdma = {};
                auto& identity = wcdma.cellIdentityWcdma.base.base;
                identity.mcc = legacyMccToString(rilInfo->CellInfo.wcdma.cellIdentityWcdma.mcc);
                identity.mnc = ril::util::mnc::decode(
                        rilInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                if (rilInfo->registered) {
                    fillMissingPlmn(identity.mcc, identity.mnc, operatorNumeric);
                }
                identity.lac = rilInfo->CellInfo.wcdma.cellIdentityWcdma.lac;
                identity.cid = rilInfo->CellInfo.wcdma.cellIdentityWcdma.cid;
                identity.psc = rilInfo->CellInfo.wcdma.cellIdentityWcdma.psc;
                identity.uarfcn = rilInfo->CellInfo.wcdma.cellIdentityWcdma.uarfcn;
                wcdma.signalStrengthWcdma.base.signalStrength =
                        rilInfo->CellInfo.wcdma.signalStrengthWcdma.signalStrength;
                wcdma.signalStrengthWcdma.base.bitErrorRate =
                        rilInfo->CellInfo.wcdma.signalStrengthWcdma.bitErrorRate;
                wcdma.signalStrengthWcdma.rscp = INT_MAX;
                wcdma.signalStrengthWcdma.ecno = INT_MAX;
                records[i].ratSpecificInfo.wcdma(wcdma);
                break;
            }
            case RIL_CELL_INFO_TYPE_CDMA: {
                V1_2::CellInfoCdma cdma = {};
                cdma.cellIdentityCdma.base.networkId =
                        rilInfo->CellInfo.cdma.cellIdentityCdma.networkId;
                cdma.cellIdentityCdma.base.systemId =
                        rilInfo->CellInfo.cdma.cellIdentityCdma.systemId;
                cdma.cellIdentityCdma.base.baseStationId =
                        rilInfo->CellInfo.cdma.cellIdentityCdma.basestationId;
                cdma.cellIdentityCdma.base.longitude =
                        rilInfo->CellInfo.cdma.cellIdentityCdma.longitude;
                cdma.cellIdentityCdma.base.latitude =
                        rilInfo->CellInfo.cdma.cellIdentityCdma.latitude;
                cdma.signalStrengthCdma.dbm = rilInfo->CellInfo.cdma.signalStrengthCdma.dbm;
                cdma.signalStrengthCdma.ecio = rilInfo->CellInfo.cdma.signalStrengthCdma.ecio;
                cdma.signalStrengthEvdo.dbm = rilInfo->CellInfo.cdma.signalStrengthEvdo.dbm;
                cdma.signalStrengthEvdo.ecio = rilInfo->CellInfo.cdma.signalStrengthEvdo.ecio;
                cdma.signalStrengthEvdo.signalNoiseRatio =
                        rilInfo->CellInfo.cdma.signalStrengthEvdo.signalNoiseRatio;
                records[i].ratSpecificInfo.cdma(cdma);
                break;
            }
            case RIL_CELL_INFO_TYPE_LTE: {
                V1_6::CellInfoLte lte = {};
                auto& identity = lte.cellIdentityLte.base.base;
                identity.mcc = legacyMccToString(rilInfo->CellInfo.lte.cellIdentityLte.mcc);
                identity.mnc = ril::util::mnc::decode(
                        rilInfo->CellInfo.lte.cellIdentityLte.mnc);
                if (rilInfo->registered) {
                    fillMissingPlmn(identity.mcc, identity.mnc, operatorNumeric);
                }
                identity.ci = rilInfo->CellInfo.lte.cellIdentityLte.ci;
                identity.pci = rilInfo->CellInfo.lte.cellIdentityLte.pci;
                identity.tac = rilInfo->CellInfo.lte.cellIdentityLte.tac;
                identity.earfcn = rilInfo->CellInfo.lte.cellIdentityLte.earfcn;
                lte.cellIdentityLte.base.bandwidth = INT_MAX;
                lte.signalStrengthLte.base.signalStrength =
                        rilInfo->CellInfo.lte.signalStrengthLte.signalStrength;
                lte.signalStrengthLte.base.rsrp = rilInfo->CellInfo.lte.signalStrengthLte.rsrp;
                lte.signalStrengthLte.base.rsrq = rilInfo->CellInfo.lte.signalStrengthLte.rsrq;
                lte.signalStrengthLte.base.rssnr = rilInfo->CellInfo.lte.signalStrengthLte.rssnr;
                lte.signalStrengthLte.base.cqi = rilInfo->CellInfo.lte.signalStrengthLte.cqi;
                lte.signalStrengthLte.base.timingAdvance =
                        rilInfo->CellInfo.lte.signalStrengthLte.timingAdvance;
                lte.signalStrengthLte.cqiTableIndex = INT_MAX;
                records[i].ratSpecificInfo.lte(lte);
                break;
            }
            case RIL_CELL_INFO_TYPE_TD_SCDMA: {
                V1_5::CellInfoTdscdma tdscdma = {};
                auto& identity = tdscdma.cellIdentityTdscdma.base.base;
                identity.mcc = legacyMccToString(
                        rilInfo->CellInfo.tdscdma.cellIdentityTdscdma.mcc);
                identity.mnc = ril::util::mnc::decode(
                        rilInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                if (rilInfo->registered) {
                    fillMissingPlmn(identity.mcc, identity.mnc, operatorNumeric);
                }
                identity.lac = rilInfo->CellInfo.tdscdma.cellIdentityTdscdma.lac;
                identity.cid = rilInfo->CellInfo.tdscdma.cellIdentityTdscdma.cid;
                identity.cpid = rilInfo->CellInfo.tdscdma.cellIdentityTdscdma.cpid;
                tdscdma.signalStrengthTdscdma.signalStrength = INT_MAX;
                tdscdma.signalStrengthTdscdma.bitErrorRate = INT_MAX;
                tdscdma.signalStrengthTdscdma.rscp =
                        rilInfo->CellInfo.tdscdma.signalStrengthTdscdma.rscp;
                records[i].ratSpecificInfo.tdscdma(tdscdma);
                break;
            }
            default:
                RLOGW("convertRilCellInfoListToHal_1_6: unsupported cell type %d",
                        rilInfo->cellInfoType);
                break;
        }
    }
}

int radio::cellInfoListInd(int slotId,
                           int indicationType, int token, RIL_Errno e, void *response,
                           size_t responseLen) {
    if (radioService[slotId] != NULL &&
            (radioService[slotId]->mRadioIndicationV1_6 != NULL ||
             radioService[slotId]->mRadioIndication != NULL)) {
        if ((response == NULL && responseLen != 0) || responseLen % sizeof(RIL_CellInfo_v12) != 0) {
            RLOGE("cellInfoListInd: invalid response");
            return 0;
        }

#if VDBG
        RLOGD("cellInfoListInd");
#endif
        Return<void> retStatus;
        if (radioService[slotId]->mRadioIndicationV1_6 != NULL) {
            hidl_vec<V1_6::CellInfo> records;
            convertRilCellInfoListToHal_1_6(slotId, response, responseLen, records);
            retStatus = radioService[slotId]->mRadioIndicationV1_6->cellInfoList_1_6(
                    convertIntToRadioIndicationType(indicationType), records);
        } else {
            hidl_vec<CellInfo> records;
            convertRilCellInfoListToHal(response, responseLen, records);
            retStatus = radioService[slotId]->mRadioIndication->cellInfoList(
                    convertIntToRadioIndicationType(indicationType), records);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cellInfoListInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::imsNetworkStateChangedInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("imsNetworkStateChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->imsNetworkStateChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("imsNetworkStateChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::subscriptionStatusChangedInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("subscriptionStatusChangedInd: invalid response");
            return 0;
        }
        bool activate = ((int32_t *) response)[0];
#if VDBG
        RLOGD("subscriptionStatusChangedInd: activate %d", activate);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->subscriptionStatusChanged(
                convertIntToRadioIndicationType(indicationType), activate);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("subscriptionStatusChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::srvccStateNotifyInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("srvccStateNotifyInd: invalid response");
            return 0;
        }
        int32_t state = ((int32_t *) response)[0];
#if VDBG
        RLOGD("srvccStateNotifyInd: rat %d", state);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->srvccStateNotify(
                convertIntToRadioIndicationType(indicationType), (SrvccState) state);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("srvccStateNotifyInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

void convertRilHardwareConfigListToHal(void *response, size_t responseLen,
        hidl_vec<HardwareConfig>& records) {
    int num = responseLen / sizeof(RIL_HardwareConfig);
    records.resize(num);

    RIL_HardwareConfig *rilHardwareConfig = (RIL_HardwareConfig *) response;
    for (int i = 0; i < num; i++) {
        records[i].type = (HardwareConfigType) rilHardwareConfig[i].type;
        records[i].uuid = convertCharPtrToHidlString(rilHardwareConfig[i].uuid);
        records[i].state = (HardwareConfigState) rilHardwareConfig[i].state;
        switch (rilHardwareConfig[i].type) {
            case RIL_HARDWARE_CONFIG_MODEM: {
                records[i].modem.resize(1);
                records[i].sim.resize(0);
                HardwareConfigModem *hwConfigModem = &records[i].modem[0];
                hwConfigModem->rat = rilHardwareConfig[i].cfg.modem.rat;
                hwConfigModem->maxVoice = rilHardwareConfig[i].cfg.modem.maxVoice;
                hwConfigModem->maxData = rilHardwareConfig[i].cfg.modem.maxData;
                hwConfigModem->maxStandby = rilHardwareConfig[i].cfg.modem.maxStandby;
                break;
            }

            case RIL_HARDWARE_CONFIG_SIM: {
                records[i].sim.resize(1);
                records[i].modem.resize(0);
                records[i].sim[0].modemUuid =
                        convertCharPtrToHidlString(rilHardwareConfig[i].cfg.sim.modemUuid);
                break;
            }
        }
    }
}

int radio::hardwareConfigChangedInd(int slotId,
                                    int indicationType, int token, RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_HardwareConfig) != 0) {
            RLOGE("hardwareConfigChangedInd: invalid response");
            return 0;
        }

        hidl_vec<HardwareConfig> configs;
        convertRilHardwareConfigListToHal(response, responseLen, configs);

#if VDBG
        RLOGD("hardwareConfigChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->hardwareConfigChanged(
                convertIntToRadioIndicationType(indicationType), configs);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hardwareConfigChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

void convertRilRadioCapabilityToHal(void *response, size_t responseLen, RadioCapability& rc) {
    RIL_RadioCapability *rilRadioCapability = (RIL_RadioCapability *) response;
    rc.session = rilRadioCapability->session;
    rc.phase = (V1_0::RadioCapabilityPhase) rilRadioCapability->phase;
    rc.raf = rilRadioCapability->rat;
    rc.logicalModemUuid = convertCharPtrToHidlString(rilRadioCapability->logicalModemUuid);
    rc.status = (V1_0::RadioCapabilityStatus) rilRadioCapability->status;
}

int radio::radioCapabilityIndicationInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_RadioCapability)) {
            RLOGE("radioCapabilityIndicationInd: invalid response");
            return 0;
        }

        RadioCapability rc = {};
        convertRilRadioCapabilityToHal(response, responseLen, rc);

#if VDBG
        RLOGD("radioCapabilityIndicationInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->radioCapabilityIndication(
                convertIntToRadioIndicationType(indicationType), rc);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("radioCapabilityIndicationInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

bool isServiceTypeCfQuery(RIL_SsServiceType serType, RIL_SsRequestType reqType) {
    if ((reqType == SS_INTERROGATION) &&
        (serType == SS_CFU ||
         serType == SS_CF_BUSY ||
         serType == SS_CF_NO_REPLY ||
         serType == SS_CF_NOT_REACHABLE ||
         serType == SS_CF_ALL ||
         serType == SS_CF_ALL_CONDITIONAL)) {
        return true;
    }
    return false;
}

int radio::onSupplementaryServiceIndicationInd(int slotId,
                                               int indicationType, int token, RIL_Errno e,
                                               void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_StkCcUnsolSsResponse)) {
            RLOGE("onSupplementaryServiceIndicationInd: invalid response");
            return 0;
        }

        RIL_StkCcUnsolSsResponse *rilSsResponse = (RIL_StkCcUnsolSsResponse *) response;
        StkCcUnsolSsResult ss = {};
        ss.serviceType = (SsServiceType) rilSsResponse->serviceType;
        ss.requestType = (SsRequestType) rilSsResponse->requestType;
        ss.teleserviceType = (SsTeleserviceType) rilSsResponse->teleserviceType;
        ss.serviceClass = rilSsResponse->serviceClass;
        ss.result = (RadioError) rilSsResponse->result;

        if (isServiceTypeCfQuery(rilSsResponse->serviceType, rilSsResponse->requestType)) {
#if VDBG
            RLOGD("onSupplementaryServiceIndicationInd CF type, num of Cf elements %d",
                    rilSsResponse->cfData.numValidIndexes);
#endif
            if (rilSsResponse->cfData.numValidIndexes > NUM_SERVICE_CLASSES) {
                RLOGE("onSupplementaryServiceIndicationInd numValidIndexes is greater than "
                        "max value %d, truncating it to max value", NUM_SERVICE_CLASSES);
                rilSsResponse->cfData.numValidIndexes = NUM_SERVICE_CLASSES;
            }

            ss.cfData.resize(1);
            ss.ssInfo.resize(0);

            /* number of call info's */
            ss.cfData[0].cfInfo.resize(rilSsResponse->cfData.numValidIndexes);

            for (int i = 0; i < rilSsResponse->cfData.numValidIndexes; i++) {
                 RIL_CallForwardInfo cf = rilSsResponse->cfData.cfInfo[i];
                 CallForwardInfo *cfInfo = &ss.cfData[0].cfInfo[i];

                 cfInfo->status = (CallForwardInfoStatus) cf.status;
                 cfInfo->reason = cf.reason;
                 cfInfo->serviceClass = cf.serviceClass;
                 cfInfo->toa = cf.toa;
                 cfInfo->number = convertCharPtrToHidlString(cf.number);
                 cfInfo->timeSeconds = cf.timeSeconds;
#if VDBG
                 RLOGD("onSupplementaryServiceIndicationInd: "
                        "Data: %d,reason=%d,cls=%d,toa=%d,num=%s,tout=%d],", cf.status,
                        cf.reason, cf.serviceClass, cf.toa, (char*)cf.number, cf.timeSeconds);
#endif
            }
        } else {
            ss.ssInfo.resize(1);
            ss.cfData.resize(0);

            /* each int */
            ss.ssInfo[0].ssInfo.resize(SS_INFO_MAX);
            for (int i = 0; i < SS_INFO_MAX; i++) {
#if VDBG
                 RLOGD("onSupplementaryServiceIndicationInd: Data: %d",
                        rilSsResponse->ssInfo[i]);
#endif
                 ss.ssInfo[0].ssInfo[i] = rilSsResponse->ssInfo[i];
            }
        }

#if VDBG
        RLOGD("onSupplementaryServiceIndicationInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->
                onSupplementaryServiceIndication(convertIntToRadioIndicationType(indicationType),
                ss);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("onSupplementaryServiceIndicationInd: "
                "radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkCallControlAlphaNotifyInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("stkCallControlAlphaNotifyInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("stkCallControlAlphaNotifyInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkCallControlAlphaNotify(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkCallControlAlphaNotifyInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

void convertRilLceDataInfoToHal(void *response, size_t responseLen, LceDataInfo& lce) {
    RIL_LceDataInfo *rilLceDataInfo = (RIL_LceDataInfo *)response;
    lce.lastHopCapacityKbps = rilLceDataInfo->last_hop_capacity_kbps;
    lce.confidenceLevel = rilLceDataInfo->confidence_level;
    lce.lceSuspended = rilLceDataInfo->lce_suspended;
}

int radio::lceDataInd(int slotId,
                      int indicationType, int token, RIL_Errno e, void *response,
                      size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_LceDataInfo)) {
            RLOGE("lceDataInd: invalid response");
            return 0;
        }

        LceDataInfo lce = {};
        convertRilLceDataInfoToHal(response, responseLen, lce);
#if VDBG
        RLOGD("lceDataInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->lceData(
                convertIntToRadioIndicationType(indicationType), lce);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("lceDataInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::pcoDataInd(int slotId,
                      int indicationType, int token, RIL_Errno e, void *response,
                      size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_PCO_Data)) {
            RLOGE("pcoDataInd: invalid response");
            return 0;
        }

        PcoDataInfo pco = {};
        RIL_PCO_Data *rilPcoData = (RIL_PCO_Data *)response;
        pco.cid = rilPcoData->cid;
        pco.bearerProto = convertCharPtrToHidlString(rilPcoData->bearer_proto);
        pco.pcoId = rilPcoData->pco_id;
        pco.contents.setToExternal((uint8_t *) rilPcoData->contents, rilPcoData->contents_length);

#if VDBG
        RLOGD("pcoDataInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->pcoData(
                convertIntToRadioIndicationType(indicationType), pco);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("pcoDataInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::modemResetInd(int slotId,
                         int indicationType, int token, RIL_Errno e, void *response,
                         size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("modemResetInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("modemResetInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->modemReset(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("modemResetInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::networkScanResultInd(int slotId,
                                int indicationType, int token, RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
    RLOGD("networkScanResultInd");
#endif
    if (radioService[slotId] != NULL &&
            (radioService[slotId]->mRadioIndicationV1_6 != NULL ||
             radioService[slotId]->mRadioIndicationV1_1 != NULL)) {
        if (response == NULL || responseLen == 0) {
            RLOGE("networkScanResultInd: invalid response");
            return 0;
        }
        RLOGD("networkScanResultInd");

#if VDBG
        RLOGD("networkScanResultInd");
#endif

        RIL_NetworkScanResult *networkScanResult = (RIL_NetworkScanResult *) response;

        Return<void> retStatus;
        if (radioService[slotId]->mRadioIndicationV1_6 != NULL) {
            V1_6::NetworkScanResult result = {};
            result.status = static_cast<V1_1::ScanStatus>(networkScanResult->status);
            result.error = static_cast<V1_6::RadioError>(networkScanResult->error);
            convertRilCellInfoListToHal_1_6(slotId, networkScanResult->network_infos,
                    networkScanResult->network_infos_length * sizeof(RIL_CellInfo_v12),
                    result.networkInfos);
            retStatus = radioService[slotId]->mRadioIndicationV1_6->networkScanResult_1_6(
                    convertIntToRadioIndicationType(indicationType), result);
        } else {
            V1_1::NetworkScanResult result = {};
            result.status = static_cast<V1_1::ScanStatus>(networkScanResult->status);
            result.error = static_cast<RadioError>(networkScanResult->error);
            convertRilCellInfoListToHal(networkScanResult->network_infos,
                    networkScanResult->network_infos_length * sizeof(RIL_CellInfo_v12),
                    result.networkInfos);
            retStatus = radioService[slotId]->mRadioIndicationV1_1->networkScanResult(
                    convertIntToRadioIndicationType(indicationType), result);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("networkScanResultInd: radioService[%d]->mRadioIndicationV1_1 == NULL", slotId);
    }
    return 0;
}

int radio::carrierInfoForImsiEncryption(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndicationV1_1 != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("carrierInfoForImsiEncryption: invalid response");
            return 0;
        }
        RLOGD("carrierInfoForImsiEncryption");
        Return<void> retStatus = radioService[slotId]->mRadioIndicationV1_1->
                carrierInfoForImsiEncryption(convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("carrierInfoForImsiEncryption: radioService[%d]->mRadioIndicationV1_1 == NULL",
                slotId);
    }

    return 0;
}

int radio::keepaliveStatusInd(int slotId,
                         int indicationType, int token, RIL_Errno e, void *response,
                         size_t responseLen) {
#if VDBG
    RLOGD("%s(): token=%d", __FUNCTION__, token);
#endif
    if (radioService[slotId] == NULL || radioService[slotId]->mRadioIndication == NULL) {
        RLOGE("%s: radioService[%d]->mRadioIndication == NULL", __FUNCTION__, slotId);
        return 0;
    }

    auto ret = V1_1::IRadioIndication::castFrom(
        radioService[slotId]->mRadioIndication);
    if (!ret.isOk()) {
        RLOGE("%s: ret.isOk() == false for radioService[%d]", __FUNCTION__, slotId);
        return 0;
    }
    sp<V1_1::IRadioIndication> radioIndicationV1_1 = ret;

    if (response == NULL || responseLen != sizeof(V1_1::KeepaliveStatus)) {
        RLOGE("%s: invalid response", __FUNCTION__);
        return 0;
    }

    V1_1::KeepaliveStatus ks;
    convertRilKeepaliveStatusToHal(static_cast<RIL_KeepaliveStatus*>(response), ks);

    Return<void> retStatus = radioIndicationV1_1->keepaliveStatus(
            convertIntToRadioIndicationType(indicationType), ks);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::oemHookRawInd(int slotId,
                         int indicationType, int token, RIL_Errno e, void *response,
                         size_t responseLen) {
    if (!kOemHookEnabled) return 0;

    if (oemHookService[slotId] != NULL && oemHookService[slotId]->mOemHookIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("oemHookRawInd: invalid response");
            return 0;
        }

        hidl_vec<uint8_t> data;
        data.setToExternal((uint8_t *) response, responseLen);
#if VDBG
        RLOGD("oemHookRawInd");
#endif
        Return<void> retStatus = oemHookService[slotId]->mOemHookIndication->oemHookRaw(
                convertIntToRadioIndicationType(indicationType), data);
        checkReturnStatus(slotId, retStatus, false);
    } else {
        RLOGE("oemHookRawInd: oemHookService[%d]->mOemHookIndication == NULL", slotId);
    }

    return 0;
}


// -----------------------------------------------------------------------------
// Samsung legacy RIL -> HIDL Radio 1.6 compatibility facade.
// The vendor RIL remains authoritative. Requests that have a safe legacy
// equivalent are downgraded; capabilities absent from legacy Samsung modems
// return REQUEST_NOT_SUPPORTED instead of synthetic success.
// -----------------------------------------------------------------------------

static RadioTechnology legacyRadioTechnology(V1_2::AccessNetwork network) {
    switch (network) {
        case V1_2::AccessNetwork::GERAN: return RadioTechnology::GSM;
        case V1_2::AccessNetwork::UTRAN: return RadioTechnology::UMTS;
        case V1_2::AccessNetwork::EUTRAN: return RadioTechnology::LTE;
        case V1_2::AccessNetwork::CDMA2000: return RadioTechnology::ONE_X_RTT;
        case V1_2::AccessNetwork::IWLAN: return RadioTechnology::IWLAN;
        default: return RadioTechnology::UNKNOWN;
    }
}

static RadioTechnology legacyRadioTechnology(V1_5::AccessNetwork network) {
    switch (network) {
        case V1_5::AccessNetwork::GERAN: return RadioTechnology::GSM;
        case V1_5::AccessNetwork::UTRAN: return RadioTechnology::UMTS;
        case V1_5::AccessNetwork::EUTRAN: return RadioTechnology::LTE;
        case V1_5::AccessNetwork::CDMA2000: return RadioTechnology::ONE_X_RTT;
        case V1_5::AccessNetwork::IWLAN: return RadioTechnology::IWLAN;
        default: return RadioTechnology::UNKNOWN;
    }
}

static RadioTechnology legacyRadioTechnologyForDataCall(
        int slotId, V1_5::AccessNetwork network) {
    const int registeredRat = getCachedDataRadioTechnology(slotId);
    switch (network) {
        case V1_5::AccessNetwork::GERAN:
            if (registeredRat == RADIO_TECH_GPRS || registeredRat == RADIO_TECH_EDGE) {
                return static_cast<RadioTechnology>(registeredRat);
            }
            // The v7 RIL setup-data ABI adds two to this enum. GPRS therefore
            // becomes the legacy string "3". Returning modern GSM (16) would
            // become "18", which the XMM626x Samsung blob rejects immediately.
            return RadioTechnology::GPRS;
        case V1_5::AccessNetwork::UTRAN:
            if (registeredRat == RADIO_TECH_UMTS || registeredRat == RADIO_TECH_HSDPA ||
                    registeredRat == RADIO_TECH_HSUPA || registeredRat == RADIO_TECH_HSPA ||
                    registeredRat == RADIO_TECH_HSPAP) {
                return static_cast<RadioTechnology>(registeredRat);
            }
            return RadioTechnology::UMTS;
        case V1_5::AccessNetwork::EUTRAN:
            return static_cast<RadioTechnology>(
                    registeredRat == RADIO_TECH_LTE_CA ? RADIO_TECH_LTE_CA : RADIO_TECH_LTE);
        default:
            return legacyRadioTechnology(network);
    }
}

static RadioTechnology legacyRadioTechnologyForDataCall(
        int slotId, V1_2::AccessNetwork network) {
    return legacyRadioTechnologyForDataCall(
            slotId, static_cast<V1_5::AccessNetwork>(network));
}

static const char *legacyPdpProtocol(int32_t protocol) {
    switch (protocol) {
        case 0: return "IP";
        case 1: return "IPV6";
        case 2: return "IPV4V6";
        case 3: return "PPP";
        default: return ""; // Caller rejects protocols unsupported by the legacy ABI.
    }
}

static bool legacyPdpProtocolSupported(int32_t protocol) {
    return protocol >= 0 && protocol <= 3;
}

static uint32_t legacyNetworkTypeToRaf(int networkType) {
    constexpr uint32_t GSM = RAF_GPRS | RAF_EDGE | RAF_GSM;
    constexpr uint32_t WCDMA = RAF_UMTS | RAF_HSDPA | RAF_HSUPA | RAF_HSPA | RAF_HSPAP;
    constexpr uint32_t CDMA = RAF_IS95A | RAF_IS95B | RAF_1xRTT;
    constexpr uint32_t EVDO = RAF_EVDO_0 | RAF_EVDO_A | RAF_EVDO_B | RAF_EHRPD;
    constexpr uint32_t LTE = RAF_LTE | RAF_LTE_CA;
    constexpr uint32_t TD = RAF_TD_SCDMA;

    switch (networkType) {
        case PREF_NET_TYPE_GSM_WCDMA:
        case PREF_NET_TYPE_GSM_WCDMA_AUTO: return GSM | WCDMA;
        case PREF_NET_TYPE_GSM_ONLY: return GSM;
        case PREF_NET_TYPE_WCDMA: return WCDMA;
        case PREF_NET_TYPE_CDMA_EVDO_AUTO: return CDMA | EVDO;
        case PREF_NET_TYPE_CDMA_ONLY: return CDMA;
        case PREF_NET_TYPE_EVDO_ONLY: return EVDO;
        case PREF_NET_TYPE_GSM_WCDMA_CDMA_EVDO_AUTO: return GSM | WCDMA | CDMA | EVDO;
        case PREF_NET_TYPE_LTE_CDMA_EVDO: return LTE | CDMA | EVDO;
        case PREF_NET_TYPE_LTE_GSM_WCDMA: return LTE | GSM | WCDMA;
        case PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA: return LTE | GSM | WCDMA | CDMA | EVDO;
        case PREF_NET_TYPE_LTE_ONLY: return LTE;
        case PREF_NET_TYPE_LTE_WCDMA: return LTE | WCDMA;
        case PREF_NET_TYPE_TD_SCDMA_ONLY: return TD;
        case PREF_NET_TYPE_TD_SCDMA_WCDMA: return TD | WCDMA;
        case PREF_NET_TYPE_TD_SCDMA_LTE: return TD | LTE;
        case PREF_NET_TYPE_TD_SCDMA_GSM: return TD | GSM;
        case PREF_NET_TYPE_TD_SCDMA_GSM_LTE: return TD | GSM | LTE;
        case PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA: return TD | GSM | WCDMA;
        case PREF_NET_TYPE_TD_SCDMA_WCDMA_LTE: return TD | WCDMA | LTE;
        case PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA_LTE: return TD | GSM | WCDMA | LTE;
        case PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA_CDMA_EVDO_AUTO:
            return TD | GSM | WCDMA | CDMA | EVDO;
        case PREF_NET_TYPE_TD_SCDMA_LTE_CDMA_EVDO_GSM_WCDMA:
            return TD | LTE | CDMA | EVDO | GSM | WCDMA;
        default: return 0;
    }
}

static bool rafToLegacyNetworkType(uint32_t raf, int *networkType) {
    constexpr uint32_t GSM = RAF_GPRS | RAF_EDGE | RAF_GSM;
    constexpr uint32_t WCDMA = RAF_UMTS | RAF_HSDPA | RAF_HSUPA | RAF_HSPA | RAF_HSPAP;
    constexpr uint32_t CDMA = RAF_IS95A | RAF_IS95B | RAF_1xRTT;
    constexpr uint32_t EVDO = RAF_EVDO_0 | RAF_EVDO_A | RAF_EVDO_B | RAF_EHRPD;
    constexpr uint32_t LTE = RAF_LTE | RAF_LTE_CA;
    constexpr uint32_t TD = RAF_TD_SCDMA;
    constexpr uint32_t NR = 1u << 20;
    constexpr uint32_t KNOWN = GSM | WCDMA | CDMA | EVDO | LTE | TD;

    if (networkType == NULL) return false;

    // Android 16 may include newer access families in its allowed-network
    // bitmap. Translate only capabilities present in the selected Samsung
    // modem generation instead of rejecting the entire request or sending an
    // unsupported mode to the proprietary RIL.
    raf &= ~NR;
#ifndef SAMSUNG_NEXT_GEN_MODEM
    raf &= GSM | WCDMA;
#endif
    if (raf == 0 || (raf & ~KNOWN) != 0) return false;

    const bool gsm = (raf & GSM) != 0;
    const bool wcdma = (raf & WCDMA) != 0;
    const bool cdma = (raf & CDMA) != 0;
    const bool evdo = (raf & EVDO) != 0;
    const bool lte = (raf & LTE) != 0;
    const bool td = (raf & TD) != 0;

    if (td) {
        if (lte && cdma && evdo && gsm && wcdma) *networkType = PREF_NET_TYPE_TD_SCDMA_LTE_CDMA_EVDO_GSM_WCDMA;
        else if (!lte && cdma && evdo && gsm && wcdma) *networkType = PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA_CDMA_EVDO_AUTO;
        else if (lte && gsm && wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA_LTE;
        else if (lte && wcdma && !gsm && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_WCDMA_LTE;
        else if (!lte && gsm && wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA;
        else if (lte && gsm && !wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_GSM_LTE;
        else if (!lte && gsm && !wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_GSM;
        else if (lte && !gsm && !wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_LTE;
        else if (!lte && !gsm && wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_WCDMA;
        else if (!lte && !gsm && !wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_TD_SCDMA_ONLY;
        else return false;
        return true;
    }

    if (lte && cdma && evdo && gsm && wcdma) *networkType = PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA;
    else if (lte && cdma && evdo && !gsm && !wcdma) *networkType = PREF_NET_TYPE_LTE_CDMA_EVDO;
    else if (lte && gsm && wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_LTE_GSM_WCDMA;
    else if (lte && !gsm && wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_LTE_WCDMA;
    else if (lte && !gsm && !wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_LTE_ONLY;
    else if (!lte && gsm && wcdma && cdma && evdo) *networkType = PREF_NET_TYPE_GSM_WCDMA_CDMA_EVDO_AUTO;
    else if (!lte && !gsm && !wcdma && cdma && evdo) *networkType = PREF_NET_TYPE_CDMA_EVDO_AUTO;
    else if (!lte && !gsm && !wcdma && cdma && !evdo) *networkType = PREF_NET_TYPE_CDMA_ONLY;
    else if (!lte && !gsm && !wcdma && !cdma && evdo) *networkType = PREF_NET_TYPE_EVDO_ONLY;
    else if (!lte && gsm && wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_GSM_WCDMA;
    else if (!lte && gsm && !wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_GSM_ONLY;
    else if (!lte && !gsm && wcdma && !cdma && !evdo) *networkType = PREF_NET_TYPE_WCDMA;
    else return false;
    return true;
}

static DataProfileInfo legacyProfile(const V1_4::DataProfileInfo& in) {
    DataProfileInfo out = {};
    // Modern frameworks use INVALID for transient APN profiles. The old
    // string ABI needs DEFAULT (0), not the negative sentinel.
    out.profileId = static_cast<int32_t>(in.profileId) < 0
            ? DataProfileId::DEFAULT : static_cast<DataProfileId>(in.profileId);
    out.apn = in.apn;
    out.protocol = legacyPdpProtocol(static_cast<int32_t>(in.protocol));
    out.roamingProtocol = legacyPdpProtocol(static_cast<int32_t>(in.roamingProtocol));
    out.authType = static_cast<ApnAuthType>(in.authType);
    out.user = in.user;
    out.password = in.password;
    out.type = static_cast<DataProfileInfoType>(in.type);
    out.maxConnsTime = in.maxConnsTime;
    out.maxConns = in.maxConns;
    out.waitTime = in.waitTime;
    out.enabled = in.enabled;
    out.supportedApnTypesBitmap = static_cast<uint32_t>(in.supportedApnTypesBitmap);
    out.bearerBitmap = static_cast<uint32_t>(in.bearerBitmap);
    out.mtu = in.mtu;
    out.mvnoType = MvnoType::NONE;
    out.mvnoMatchData = "";
    return out;
}

static DataProfileInfo legacyProfile(const V1_5::DataProfileInfo& in) {
    DataProfileInfo out = {};
    // Modern frameworks use INVALID for transient APN profiles. The old
    // string ABI needs DEFAULT (0), not the negative sentinel.
    out.profileId = static_cast<int32_t>(in.profileId) < 0
            ? DataProfileId::DEFAULT : static_cast<DataProfileId>(in.profileId);
    out.apn = in.apn;
    out.protocol = legacyPdpProtocol(static_cast<int32_t>(in.protocol));
    out.roamingProtocol = legacyPdpProtocol(static_cast<int32_t>(in.roamingProtocol));
    out.authType = static_cast<ApnAuthType>(in.authType);
    out.user = in.user;
    out.password = in.password;
    out.type = static_cast<DataProfileInfoType>(in.type);
    out.maxConnsTime = in.maxConnsTime;
    out.maxConns = in.maxConns;
    out.waitTime = in.waitTime;
    out.enabled = in.enabled;
    out.supportedApnTypesBitmap = static_cast<uint32_t>(in.supportedApnTypesBitmap);
    out.bearerBitmap = static_cast<uint32_t>(in.bearerBitmap);
    out.mtu = std::max(in.mtuV4, in.mtuV6);
    out.mvnoType = MvnoType::NONE;
    out.mvnoMatchData = "";
    return out;
}

static V1_0::RadioResponseInfo unsupportedInfo(int32_t serial) {
    V1_0::RadioResponseInfo info = {};
    info.type = V1_0::RadioResponseType::SOLICITED;
    info.serial = serial;
    info.error = V1_0::RadioError::REQUEST_NOT_SUPPORTED;
    return info;
}

static V1_6::RadioResponseInfo unsupportedInfo_1_6(int32_t serial) {
    V1_6::RadioResponseInfo info = {};
    info.type = V1_0::RadioResponseType::SOLICITED;
    info.serial = serial;
    info.error = V1_6::RadioError::REQUEST_NOT_SUPPORTED;
    return info;
}

static Return<void> rejectLegacyRequest(int32_t serial, int slotId, int request,
                                        RIL_Errno error = RIL_E_REQUEST_NOT_SUPPORTED) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI != NULL) sendErrorResponse(pRI, error);
    return Void();
}

Return<void> RadioImpl::startNetworkScan_1_2(int32_t serial, const V1_2::NetworkScanRequest&) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_1 != nullptr) {
        auto ret = mRadioResponseV1_1->startNetworkScanResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setIndicationFilter_1_2(int32_t serial,
        hidl_bitfield<V1_2::IndicationFilter> indicationFilter) {
    return setIndicationFilter(serial, static_cast<int32_t>(indicationFilter));
}

Return<void> RadioImpl::setSignalStrengthReportingCriteria(int32_t serial, int32_t, int32_t,
        const hidl_vec<int32_t>&, V1_2::AccessNetwork) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_2 != nullptr) {
        auto ret = mRadioResponseV1_2->setSignalStrengthReportingCriteriaResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setLinkCapacityReportingCriteria(int32_t serial, int32_t, int32_t, int32_t,
        const hidl_vec<int32_t>&, const hidl_vec<int32_t>&, V1_2::AccessNetwork) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_2 != nullptr) {
        auto ret = mRadioResponseV1_2->setLinkCapacityReportingCriteriaResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setupDataCall_1_2(int32_t serial, V1_2::AccessNetwork accessNetwork,
        const DataProfileInfo& dataProfileInfo, bool modemCognitive, bool roamingAllowed, bool isRoaming,
        V1_2::DataRequestReason reason, const hidl_vec<hidl_string>&, const hidl_vec<hidl_string>&) {
    if (reason == V1_2::DataRequestReason::HANDOVER ||
            legacyRadioTechnologyForDataCall(mSlotId, accessNetwork) == RadioTechnology::UNKNOWN) {
        return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL);
    }
    return setupDataCall(serial, legacyRadioTechnologyForDataCall(mSlotId, accessNetwork), dataProfileInfo,
            modemCognitive, roamingAllowed, isRoaming);
}

Return<void> RadioImpl::deactivateDataCall_1_2(int32_t serial, int32_t cid, V1_2::DataRequestReason reason) {
    return deactivateDataCall(serial, cid,
            reason == V1_2::DataRequestReason::SHUTDOWN);
}

Return<void> RadioImpl::setSystemSelectionChannels(int32_t serial, bool,
        const hidl_vec<V1_1::RadioAccessSpecifier>&) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_3 != nullptr) {
        auto ret = mRadioResponseV1_3->setSystemSelectionChannelsResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::enableModem(int32_t serial, bool) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_3 != nullptr) {
        auto ret = mRadioResponseV1_3->enableModemResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getModemStackStatus(int32_t serial) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_3 != nullptr) {
        auto ret = mRadioResponseV1_3->getModemStackStatusResponse(info, true);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setupDataCall_1_4(int32_t serial, V1_4::AccessNetwork accessNetwork,
        const V1_4::DataProfileInfo& dataProfileInfo, bool roamingAllowed, V1_2::DataRequestReason reason,
        const hidl_vec<hidl_string>&, const hidl_vec<hidl_string>&) {
    DataProfileInfo p = legacyProfile(dataProfileInfo);
    if (reason == V1_2::DataRequestReason::HANDOVER ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.protocol)) ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.roamingProtocol)) ||
            legacyRadioTechnologyForDataCall(mSlotId,
                    static_cast<V1_2::AccessNetwork>(accessNetwork)) ==
                    RadioTechnology::UNKNOWN) {
        return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL);
    }
    return setupDataCall(serial, legacyRadioTechnologyForDataCall(mSlotId,
            static_cast<V1_2::AccessNetwork>(accessNetwork)),
            p, false, roamingAllowed, false);
}

Return<void> RadioImpl::setInitialAttachApn_1_4(int32_t serial,
        const V1_4::DataProfileInfo& dataProfileInfo) {
    if (!legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.protocol)) ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.roamingProtocol))) {
        return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SET_INITIAL_ATTACH_APN);
    }
    DataProfileInfo p = legacyProfile(dataProfileInfo);
    return setInitialAttachApn(serial, p, false, false);
}

Return<void> RadioImpl::setDataProfile_1_4(int32_t serial,
        const hidl_vec<V1_4::DataProfileInfo>& profiles) {
    hidl_vec<DataProfileInfo> legacy;
    legacy.resize(profiles.size());
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (!legacyPdpProtocolSupported(static_cast<int32_t>(profiles[i].protocol)) ||
                !legacyPdpProtocolSupported(static_cast<int32_t>(profiles[i].roamingProtocol))) {
            return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SET_DATA_PROFILE);
        }
        legacy[i] = legacyProfile(profiles[i]);
    }
    return setDataProfile(serial, legacy, false);
}

Return<void> RadioImpl::emergencyDial(int32_t serial, const Dial& dialInfo,
        hidl_bitfield<V1_4::EmergencyServiceCategory>, const hidl_vec<hidl_string>&,
        V1_4::EmergencyCallRouting, bool, bool) {
    // Old Samsung RIL identifies emergency numbers in its ordinary DIAL handler.
    // Use that command while preserving the emergencyDial response contract.
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_DIAL);
    if (pRI == NULL) return Void();
    pRI->pCI = &s_emergencyDialCommand;

    RIL_Dial dial = {};
    RIL_UUS_Info uusInfo = {};
    if (!copyHidlStringToRil(&dial.address, dialInfo.address, pRI)) return Void();
    dial.clir = static_cast<int>(dialInfo.clir);
    if (dialInfo.uusInfo.size() != 0) {
        uusInfo.uusType = static_cast<RIL_UUS_Type>(dialInfo.uusInfo[0].uusType);
        uusInfo.uusDcs = static_cast<RIL_UUS_DCS>(dialInfo.uusInfo[0].uusDcs);
        if (!copyHidlStringToRil(&uusInfo.uusData, dialInfo.uusInfo[0].uusData, pRI)) {
            memsetAndFreeStrings(1, dial.address);
            return Void();
        }
        uusInfo.uusLength = dialInfo.uusInfo[0].uusData.size();
        dial.uusInfo = &uusInfo;
    }
    CALL_ONREQUEST(RIL_REQUEST_DIAL, &dial, sizeof(dial), pRI, mSlotId);
    memsetAndFreeStrings(2, dial.address, uusInfo.uusData);
    return Void();
}

Return<void> RadioImpl::startNetworkScan_1_4(int32_t serial, const V1_2::NetworkScanRequest&) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_4 != nullptr) {
        auto ret = mRadioResponseV1_4->startNetworkScanResponse_1_4(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getPreferredNetworkTypeBitmap(int32_t serial) {
    RequestInfo *pRI = android::addRequestToList(
            serial, mSlotId, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE);
    if (pRI == NULL) return Void();
    pRI->pCI = &s_getPreferredNetworkTypeBitmapCommand;
    CALL_ONREQUEST(RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE, NULL, 0, pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::setPreferredNetworkTypeBitmap(int32_t serial,
        hidl_bitfield<V1_4::RadioAccessFamily> networkTypeBitmap) {
    int legacyType;
    if (!rafToLegacyNetworkType(static_cast<uint32_t>(networkTypeBitmap), &legacyType)) {
        auto info = unsupportedInfo(serial);
        info.error = RadioError::INVALID_ARGUMENTS;
        if (mRadioResponseV1_4 != nullptr) {
            auto ret = mRadioResponseV1_4->setPreferredNetworkTypeBitmapResponse(info);
            checkReturnStatus(ret);
        }
        return Void();
    }
    RequestInfo *pRI = android::addRequestToList(
            serial, mSlotId, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
    if (pRI == NULL) return Void();
    pRI->pCI = &s_setPreferredNetworkTypeBitmapCommand;
    CALL_ONREQUEST(RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, &legacyType,
            sizeof(legacyType), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::setAllowedCarriers_1_4(int32_t serial,
        const V1_4::CarrierRestrictionsWithPriority&, V1_4::SimLockMultiSimPolicy) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_4 != nullptr) {
        auto ret = mRadioResponseV1_4->setAllowedCarriersResponse_1_4(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getAllowedCarriers_1_4(int32_t serial) {
    auto info = unsupportedInfo(serial);
    V1_4::CarrierRestrictionsWithPriority carriers = {};
    if (mRadioResponseV1_4 != nullptr) {
        auto ret = mRadioResponseV1_4->getAllowedCarriersResponse_1_4(info, carriers,
                V1_4::SimLockMultiSimPolicy::NO_MULTISIM_POLICY);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getSignalStrength_1_4(int32_t serial) {
    return getSignalStrength(serial);
}

Return<void> RadioImpl::setSignalStrengthReportingCriteria_1_5(int32_t serial,
        const V1_5::SignalThresholdInfo&, V1_5::AccessNetwork) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_5 != nullptr) {
        auto ret = mRadioResponseV1_5->setSignalStrengthReportingCriteriaResponse_1_5(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setLinkCapacityReportingCriteria_1_5(int32_t serial, int32_t, int32_t, int32_t,
        const hidl_vec<int32_t>&, const hidl_vec<int32_t>&, V1_5::AccessNetwork) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_5 != nullptr) {
        auto ret = mRadioResponseV1_5->setLinkCapacityReportingCriteriaResponse_1_5(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::enableUiccApplications(int32_t serial, bool enable) {
#ifndef SAMSUNG_NEXT_GEN_MODEM
    /*
     * XMM6260/6262/6360 cannot independently disable UICC applications, and
     * Samsung's private SET_SIM_POWER is fire-and-forget and can strand the CP
     * in BOOTING. On a single-SIM legacy target, radio power is the closest
     * safe and observable equivalent: the blob completes it, service really
     * stops, and the modem can be enabled again without a CP cold boot.
     */
    RLOGI("enableUiccApplications: slot %d enabled=%d via RADIO_POWER compatibility",
            mSlotId, enable);
    RequestInfo *pRI = android::addRequestToList(
            serial, mSlotId, RIL_REQUEST_RADIO_POWER);
    if (pRI == nullptr) return Void();
    pRI->pCI = &s_enableUiccApplicationsCommand;
    rememberUiccApplicationsRequest(mSlotId, serial, enable);
    int radioPower = BOOL_TO_INT(enable);
    CALL_ONREQUEST(RIL_REQUEST_RADIO_POWER, &radioPower, sizeof(radioPower), pRI, mSlotId);
    return Void();
#else
    // Newer Samsung blobs may have a different UICC power contract. Keep the
    // operation explicit instead of applying the XMM626x radio-power mapping.
    RLOGW("enableUiccApplications: slot %d enabled=%d has no verified next-gen mapping",
            mSlotId, enable);
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_5 != nullptr) {
        auto ret = mRadioResponseV1_5->enableUiccApplicationsResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
#endif
}

Return<void> RadioImpl::areUiccApplicationsEnabled(int32_t serial) {
    RadioResponseInfo info = {};
    info.type = RadioResponseType::SOLICITED;
    info.serial = serial;
    info.error = RadioError::NONE;
    if (mRadioResponseV1_5 != nullptr) {
        auto ret = mRadioResponseV1_5->areUiccApplicationsEnabledResponse(
                info, getUiccApplicationsEnabled(mSlotId));
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setSystemSelectionChannels_1_5(int32_t serial, bool,
        const hidl_vec<V1_5::RadioAccessSpecifier>&) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_5 != nullptr) {
        auto ret = mRadioResponseV1_5->setSystemSelectionChannelsResponse_1_5(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::startNetworkScan_1_5(int32_t serial, const V1_5::NetworkScanRequest&) {
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_5 != nullptr) {
        auto ret = mRadioResponseV1_5->startNetworkScanResponse_1_5(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setupDataCall_1_5(int32_t serial, V1_5::AccessNetwork accessNetwork,
        const V1_5::DataProfileInfo& dataProfileInfo, bool roamingAllowed, V1_2::DataRequestReason reason,
        const hidl_vec<V1_5::LinkAddress>&, const hidl_vec<hidl_string>&) {
    DataProfileInfo p = legacyProfile(dataProfileInfo);
    if (reason == V1_2::DataRequestReason::HANDOVER ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.protocol)) ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.roamingProtocol)) ||
            legacyRadioTechnologyForDataCall(mSlotId, accessNetwork) == RadioTechnology::UNKNOWN) {
        return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL);
    }
    return setupDataCall(serial, legacyRadioTechnologyForDataCall(mSlotId, accessNetwork),
            p, false, roamingAllowed, false);
}

Return<void> RadioImpl::setInitialAttachApn_1_5(int32_t serial,
        const V1_5::DataProfileInfo& dataProfileInfo) {
    if (!legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.protocol)) ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.roamingProtocol))) {
        return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SET_INITIAL_ATTACH_APN);
    }
    DataProfileInfo p = legacyProfile(dataProfileInfo);
    return setInitialAttachApn(serial, p, false, false);
}

Return<void> RadioImpl::setDataProfile_1_5(int32_t serial,
        const hidl_vec<V1_5::DataProfileInfo>& profiles) {
    hidl_vec<DataProfileInfo> legacy;
    legacy.resize(profiles.size());
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (!legacyPdpProtocolSupported(static_cast<int32_t>(profiles[i].protocol)) ||
                !legacyPdpProtocolSupported(static_cast<int32_t>(profiles[i].roamingProtocol))) {
            return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SET_DATA_PROFILE);
        }
        legacy[i] = legacyProfile(profiles[i]);
    }
    return setDataProfile(serial, legacy, false);
}

Return<void> RadioImpl::setRadioPower_1_5(int32_t serial, bool powerOn, bool, bool) {
    return setRadioPower(serial, powerOn);
}

Return<void> RadioImpl::setIndicationFilter_1_5(int32_t serial,
        hidl_bitfield<V1_5::IndicationFilter> indicationFilter) {
    return setIndicationFilter(serial, static_cast<int32_t>(indicationFilter));
}

Return<void> RadioImpl::getBarringInfo(int32_t serial) {
    auto info = unsupportedInfo(serial);
    V1_5::CellIdentity cellIdentity = {};
    hidl_vec<V1_5::BarringInfo> barringInfos;
    if (mRadioResponseV1_5 != nullptr) {
        auto ret = mRadioResponseV1_5->getBarringInfoResponse(info, cellIdentity, barringInfos);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getVoiceRegistrationState_1_5(int32_t serial) {
    return getVoiceRegistrationState(serial);
}

Return<void> RadioImpl::getDataRegistrationState_1_5(int32_t serial) {
    return getDataRegistrationState(serial);
}

Return<void> RadioImpl::setNetworkSelectionModeManual_1_5(int32_t serial,
        const hidl_string& operatorNumeric, V1_5::RadioAccessNetworks) {
    return setNetworkSelectionModeManual(serial, operatorNumeric);
}

Return<void> RadioImpl::sendCdmaSmsExpectMore(int32_t serial, const CdmaSmsMessage& sms) {
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_CDMA_SEND_SMS);
    if (pRI == NULL) return Void();
    pRI->pCI = &s_cdmaSmsExpectMoreCommand;
    RIL_CDMA_SMS_Message message = {};
    constructCdmaSms(message, sms);
    CALL_ONREQUEST(RIL_REQUEST_CDMA_SEND_SMS, &message, sizeof(message), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::supplySimDepersonalization(int32_t serial, V1_5::PersoSubstate persoType,
        const hidl_string& controlKey) {
    if (persoType != V1_5::PersoSubstate::SIM_NETWORK) {
        auto info = unsupportedInfo(serial);
        if (mRadioResponseV1_5 != nullptr) {
            auto ret = mRadioResponseV1_5->supplySimDepersonalizationResponse(
                    info, persoType, -1);
            checkReturnStatus(ret);
        }
        return Void();
    }
    RequestInfo *pRI = android::addRequestToList(
            serial, mSlotId, RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION);
    if (pRI == NULL) return Void();
    pRI->pCI = &s_supplySimDepersonalizationCommand;
    char *key = NULL;
    if (!copyHidlStringToRil(&key, controlKey, pRI)) return Void();
    char *args[] = {key};
    CALL_ONREQUEST(RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION,
            args, sizeof(args), pRI, mSlotId);
    memsetAndFreeStrings(1, key);
    return Void();
}

Return<void> RadioImpl::setRadioPower_1_6(int32_t serial, bool powerOn, bool, bool) {
    return setRadioPower(serial, powerOn);
}

Return<void> RadioImpl::getDataCallList_1_6(int32_t serial) {
    return getDataCallList(serial);
}

Return<void> RadioImpl::setupDataCall_1_6(int32_t serial, V1_5::AccessNetwork accessNetwork,
        const V1_5::DataProfileInfo& dataProfileInfo, bool roamingAllowed, V1_2::DataRequestReason reason,
        const hidl_vec<V1_5::LinkAddress>&, const hidl_vec<hidl_string>&, int32_t pduSessionId,
        const V1_6::OptionalSliceInfo& sliceInfo,
        const V1_6::OptionalTrafficDescriptor& trafficDescriptor, bool) {
    bool compatibleTrafficDescriptor = true;
    if (trafficDescriptor.getDiscriminator() ==
            V1_6::OptionalTrafficDescriptor::hidl_discriminator::value) {
        const V1_6::TrafficDescriptor& descriptor = trafficDescriptor.value();
        const bool hasDnn = descriptor.dnn.getDiscriminator() ==
                V1_6::OptionalDnn::hidl_discriminator::value;
        const bool hasOsAppId = descriptor.osAppId.getDiscriminator() ==
                V1_6::OptionalOsAppId::hidl_discriminator::value;

        // Android 16 supplies a DNN-only descriptor that repeats the APN even
        // for non-sliced legacy data calls. It carries no information that
        // the v7 ABI cannot represent, so downgrade it after validating it.
        compatibleTrafficDescriptor = hasDnn && !hasOsAppId &&
                !descriptor.dnn.value().empty() &&
                descriptor.dnn.value() == dataProfileInfo.apn;
        if (compatibleTrafficDescriptor) {
            RLOGD("setupDataCall_1_6: accepting APN-equivalent DNN '%s' for legacy RIL",
                    descriptor.dnn.value().c_str());
        }
    }

    DataProfileInfo p = legacyProfile(dataProfileInfo);
    if (reason == V1_2::DataRequestReason::HANDOVER || pduSessionId != 0 ||
            sliceInfo.getDiscriminator() ==
                    V1_6::OptionalSliceInfo::hidl_discriminator::value ||
            !compatibleTrafficDescriptor ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.protocol)) ||
            !legacyPdpProtocolSupported(static_cast<int32_t>(dataProfileInfo.roamingProtocol)) ||
            legacyRadioTechnologyForDataCall(mSlotId, accessNetwork) == RadioTechnology::UNKNOWN) {
        return rejectLegacyRequest(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL);
    }
    return setupDataCall(serial, legacyRadioTechnologyForDataCall(mSlotId, accessNetwork),
            p, false, roamingAllowed, false);
}

Return<void> RadioImpl::sendSms_1_6(int32_t serial, const GsmSmsMessage& message) {
    return sendSms(serial, message);
}

Return<void> RadioImpl::sendSmsExpectMore_1_6(int32_t serial, const GsmSmsMessage& message) {
    return sendSMSExpectMore(serial, message);
}

Return<void> RadioImpl::sendCdmaSms_1_6(int32_t serial, const CdmaSmsMessage& sms) {
    return sendCdmaSms(serial, sms);
}

Return<void> RadioImpl::sendCdmaSmsExpectMore_1_6(int32_t serial, const CdmaSmsMessage& sms) {
    return sendCdmaSmsExpectMore(serial, sms);
}

Return<void> RadioImpl::setSimCardPower_1_6(int32_t serial, V1_1::CardPowerState powerUp) {
    return setSimCardPower_1_1(serial, powerUp);
}

Return<void> RadioImpl::setNrDualConnectivityState(int32_t serial, V1_6::NrDualConnectivityState) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->setNrDualConnectivityStateResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::isNrDualConnectivityEnabled(int32_t serial) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->isNrDualConnectivityEnabledResponse(info, false);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::allocatePduSessionId(int32_t serial) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->allocatePduSessionIdResponse(info, 0);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::releasePduSessionId(int32_t serial, int32_t) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->releasePduSessionIdResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::startHandover(int32_t serial, int32_t) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->startHandoverResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::cancelHandover(int32_t serial, int32_t) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->cancelHandoverResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setAllowedNetworkTypesBitmap(uint32_t serial,
        hidl_bitfield<V1_4::RadioAccessFamily> networkTypeBitmap) {
    int legacyType;
    if (!rafToLegacyNetworkType(static_cast<uint32_t>(networkTypeBitmap), &legacyType)) {
        auto info = unsupportedInfo_1_6(static_cast<int32_t>(serial));
        info.error = V1_6::RadioError::INVALID_ARGUMENTS;
        if (mRadioResponseV1_6 != nullptr) {
            auto ret = mRadioResponseV1_6->setAllowedNetworkTypesBitmapResponse(info);
            checkReturnStatus(ret);
        }
        return Void();
    }
    RLOGI("setAllowedNetworkTypesBitmap: slot %d serial %u RAF=0x%x legacyType=%d",
            mSlotId, serial, static_cast<uint32_t>(networkTypeBitmap), legacyType);
    RequestInfo *pRI = android::addRequestToList(static_cast<int32_t>(serial), mSlotId,
            RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
    if (pRI == NULL) return Void();
    pRI->pCI = &s_setAllowedNetworkTypesBitmapCommand;
    CALL_ONREQUEST(RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, &legacyType,
            sizeof(legacyType), pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::getAllowedNetworkTypesBitmap(int32_t serial) {
    RequestInfo *pRI = android::addRequestToList(
            serial, mSlotId, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE);
    if (pRI == NULL) return Void();
    pRI->pCI = &s_getAllowedNetworkTypesBitmapCommand;
    CALL_ONREQUEST(RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE, NULL, 0, pRI, mSlotId);
    return Void();
}

Return<void> RadioImpl::setDataThrottling(int32_t serial, V1_6::DataThrottlingAction, int64_t) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->setDataThrottlingResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::emergencyDial_1_6(int32_t serial, const Dial& dialInfo,
        hidl_bitfield<V1_4::EmergencyServiceCategory> categories,
        const hidl_vec<hidl_string>& urns, V1_4::EmergencyCallRouting routing,
        bool hasKnownUserIntentEmergency, bool isTesting) {
    return emergencyDial(serial, dialInfo, categories, urns, routing,
            hasKnownUserIntentEmergency, isTesting);
}

Return<void> RadioImpl::getSystemSelectionChannels(int32_t serial) {
    auto info = unsupportedInfo_1_6(serial);
    hidl_vec<V1_5::RadioAccessSpecifier> specifiers;
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->getSystemSelectionChannelsResponse(info, specifiers);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getCellInfoList_1_6(int32_t serial) {
    return getCellInfoList(serial);
}

Return<void> RadioImpl::getVoiceRegistrationState_1_6(int32_t serial) {
    return getVoiceRegistrationState(serial);
}

Return<void> RadioImpl::getSignalStrength_1_6(int32_t serial) {
    return getSignalStrength(serial);
}

Return<void> RadioImpl::getDataRegistrationState_1_6(int32_t serial) {
    return getDataRegistrationState(serial);
}

Return<void> RadioImpl::getCurrentCalls_1_6(int32_t serial) {
    return getCurrentCalls(serial);
}

Return<void> RadioImpl::getSlicingConfig(int32_t serial) {
    auto info = unsupportedInfo_1_6(serial);
    V1_6::SlicingConfig config = {};
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->getSlicingConfigResponse(info, config);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::setCarrierInfoForImsiEncryption_1_6(int32_t serial,
        const V1_6::ImsiEncryptionInfo&) {
    // The 1.6 key format includes fields absent from the 1.1 legacy request. Do not truncate it.
    // This response method is inherited from IRadioResponse 1.1 and therefore
    // still takes the 1.0 RadioResponseInfo type, including on a 1.6 callback.
    auto info = unsupportedInfo(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->setCarrierInfoForImsiEncryptionResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getSimPhonebookRecords(int32_t serial) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->getSimPhonebookRecordsResponse(info);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::getSimPhonebookCapacity(int32_t serial) {
    auto info = unsupportedInfo_1_6(serial);
    V1_6::PhonebookCapacity capacity = {};
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->getSimPhonebookCapacityResponse(info, capacity);
        checkReturnStatus(ret);
    }
    return Void();
}

Return<void> RadioImpl::updateSimPhonebookRecords(int32_t serial,
        const V1_6::PhonebookRecordInfo&) {
    auto info = unsupportedInfo_1_6(serial);
    if (mRadioResponseV1_6 != nullptr) {
        auto ret = mRadioResponseV1_6->updateSimPhonebookRecordsResponse(info, -1);
        checkReturnStatus(ret);
    }
    return Void();
}


void radio::registerService(RIL_RadioFunctions *callbacks, CommandInfo *commands) {
    using namespace android::hardware;
    int simCount = 1;
    const char *serviceNames[] = {
            android::RIL_getServiceName()
            #if (SIM_COUNT >= 2)
            , RIL2_SERVICE_NAME
            #if (SIM_COUNT >= 3)
            , RIL3_SERVICE_NAME
            #if (SIM_COUNT >= 4)
            , RIL4_SERVICE_NAME
            #endif
            #endif
            #endif
            };

    #if (SIM_COUNT >= 2)
    simCount = SIM_COUNT;
    #endif

    s_vendorFunctions = callbacks;
    s_commands = commands;

    configureRpcThreadpool(1, true /* callerWillJoin */);
    for (int i = 0; i < simCount; i++) {
        pthread_rwlock_t *radioServiceRwlockPtr = getRadioServiceRwlock(i);
        int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
        assert(ret == 0);

        radioService[i] = new RadioImpl;
        radioService[i]->mSlotId = i;
        RLOGD("registerService: starting android::hardware::radio::V1_6::IRadio %s",
                serviceNames[i]);
        android::status_t status = radioService[i]->registerAsService(serviceNames[i]);

        if (kOemHookEnabled) {
            oemHookService[i] = new OemHookImpl;
            oemHookService[i]->mSlotId = i;
            status = oemHookService[i]->registerAsService(serviceNames[i]);
        }

        ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(ret == 0);
    }
}

void rilc_thread_pool() {
    joinRpcThreadpool();
}

pthread_rwlock_t * radio::getRadioServiceRwlock(int slotId) {
    pthread_rwlock_t *radioServiceRwlockPtr = &radioServiceRwlock;

    #if (SIM_COUNT >= 2)
    if (slotId == 1) radioServiceRwlockPtr = &radioServiceRwlock2;
    #if (SIM_COUNT >= 3)
    if (slotId == 2) radioServiceRwlockPtr = &radioServiceRwlock3;
    #if (SIM_COUNT >= 4)
    if (slotId == 3) radioServiceRwlockPtr = &radioServiceRwlock4;
    #endif
    #endif
    #endif

    return radioServiceRwlockPtr;
}

// should acquire write lock for the corresponding service before calling this
void radio::setNitzTimeReceived(int slotId, long timeReceived) {
    nitzTimeReceived[slotId] = timeReceived;
}
