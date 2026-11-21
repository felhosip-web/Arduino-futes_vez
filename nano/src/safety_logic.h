#pragma once

#include <math.h>
#include <stdint.h>
#include "common.h"

constexpr uint32_t NANO_RELAY_MISMATCH_TIMEOUT_MS = 500UL;

inline bool isNanoTempValid(float t) {
    return !isnan(t) && (t > -120.0f) && (t <= 125.0f);
}

inline AlarmStatePacked evaluateNanoAlarmState(
    float latestKazan,
    bool sensorUnknown,
    bool hasEspRequest,
    bool failsafeActive,
    const AlarmConfigPacked& alarmCfg
) {
    AlarmStatePacked out{ 0U, ALARM_REASON_NONE };
    bool kazanInvalid  = !isNanoTempValid(latestKazan);
    bool alarmOverheat = !kazanInvalid && (latestKazan >= alarmCfg.alarmTemp);

    if (alarmCfg.alarmEnabled == 0U) {
        out.active = 0U;
        out.reason = ALARM_REASON_NONE;
    } else if (alarmOverheat) {
        out.active = 1U;
        out.reason = ALARM_REASON_OVERHEAT;
    } else if (kazanInvalid || sensorUnknown) {
        out.active = 1U;
        out.reason = ALARM_REASON_SENSOR_FAIL;
    } else if (!hasEspRequest || failsafeActive) {
        out.active = 1U;
        out.reason = ALARM_REASON_NRF_FAIL;
    }
    return out;
}

inline uint8_t computeNanoEffectiveOutputs(
    float latestKazan,
    bool hasEspRequest,
    bool failsafeActive,
    uint8_t lastEspOutputsMask,
    bool serviceMode,
    bool loopFault,
    bool loopFaultBlinkOn,
    const AlarmConfigPacked& alarmCfg,
    const AlarmStatePacked& alarmState,
    bool alarmBlinkOn
) {
    bool kazanInvalid   = !isNanoTempValid(latestKazan);
    bool safetyOverheat = !kazanInvalid && (latestKazan >= SAFETY_OVERHEAT_TEMP);

    uint8_t effectiveMask = 0U;
    if (!hasEspRequest || failsafeActive) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
    } else {
        effectiveMask = lastEspOutputsMask;
    }

    // Biztonsági szabály: CSAK a fix SAFETY_OVERHEAT_TEMP (95.0C) vagy kazán szenzorhiba (-127) kapcsolja BE a szivattyút
    // SAFETY: alarmEnabled nem kapcsolja ki a pumpavédelmet
    if (safetyOverheat || kazanInvalid) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
    }

    // Szerviz kapcsoló (D6 aktív LOW): szivattyú AZONNAL BE az ESP parancstól függetlenül, zöld LED folyamatos BE
    if (serviceMode) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
        setBit(effectiveMask, OUT_BIT_LED_ZOLD, true);
    }

    if (loopFault) {
        setBit(effectiveMask, OUT_BIT_LED_ZOLD, loopFaultBlinkOn);
    }

    bool pirosOn = false;
    if (alarmCfg.alarmEnabled == 0U) {
        pirosOn = false;
    } else if (alarmState.active != 0U || !hasEspRequest || failsafeActive) {
        pirosOn = alarmBlinkOn;
    } else {
        pirosOn = getBit(effectiveMask, OUT_BIT_LED_PIROS);
    }

    setBit(effectiveMask, OUT_BIT_LED_PIROS, pirosOn);
    return effectiveMask;
}

enum RelayStuckLogState : uint8_t {
    STUCK_LOG_NONE = 0,
    STUCK_LOG_ON   = 1,
    STUCK_LOG_OFF  = 2
};

struct RelayMismatchState {
    bool     relayMismatchTiming;
    uint32_t relayMismatchStartMs;
    bool     relayMismatchActive;
    uint8_t  loggedStuckState;
    bool     pendingCmdVerifyActive;
    bool     lastCmdAck;
    bool     lastCmdRejectedService;
    bool     lastCmdMismatch;
};

enum RelayMismatchEvent : uint8_t {
    RELAY_EVT_NONE      = 0,
    RELAY_EVT_STUCK_ON  = 1,
    RELAY_EVT_STUCK_OFF = 2
};

inline RelayMismatchEvent stepRelayMismatch(
    RelayMismatchState& st,
    bool commandedPumpOn,
    bool relayFeedbackOn,
    uint32_t now
) {
    if (commandedPumpOn == relayFeedbackOn) {
        st.relayMismatchTiming = false;
        st.relayMismatchActive = false;
        st.loggedStuckState    = STUCK_LOG_NONE;
        if (st.pendingCmdVerifyActive) {
            st.pendingCmdVerifyActive = false;
            st.lastCmdAck             = true;
            st.lastCmdRejectedService = false;
            st.lastCmdMismatch        = false;
        }
        return RELAY_EVT_NONE;
    }

    if (!st.relayMismatchTiming) {
        st.relayMismatchTiming  = true;
        st.relayMismatchStartMs = now;
        return RELAY_EVT_NONE;
    }

    if (static_cast<uint32_t>(now - st.relayMismatchStartMs) >= NANO_RELAY_MISMATCH_TIMEOUT_MS) {
        st.relayMismatchActive = true;
        RelayMismatchEvent evt = RELAY_EVT_NONE;
        if (!commandedPumpOn && relayFeedbackOn) {
            if (st.loggedStuckState != STUCK_LOG_ON) {
                st.loggedStuckState = STUCK_LOG_ON;
                evt = RELAY_EVT_STUCK_ON;
            }
        } else if (commandedPumpOn && !relayFeedbackOn) {
            if (st.loggedStuckState != STUCK_LOG_OFF) {
                st.loggedStuckState = STUCK_LOG_OFF;
                evt = RELAY_EVT_STUCK_OFF;
            }
        }
        if (st.pendingCmdVerifyActive) {
            st.pendingCmdVerifyActive = false;
            st.lastCmdAck             = false;
            st.lastCmdRejectedService = false;
            st.lastCmdMismatch        = true;
        }
        return evt;
    }

    return RELAY_EVT_NONE;
}
