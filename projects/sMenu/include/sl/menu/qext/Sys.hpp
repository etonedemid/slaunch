#pragma once
#include <switch.h>
#ifdef SL_QEXT
#include <sl/menu/smi/QextBackend.hpp>
#endif

// System state the menu shows. The qlaunch-ext loader owns these services and
// exports them; the applet build and the simulator ask the services directly.
namespace sl::menu::sys {

    inline u32 BatteryPercent() {
#ifdef SL_QEXT
        const int p = qext_battery_percent();
        return p < 0 ? 0 : (u32)p;
#else
        u32 charge = 0;
        psmGetBatteryChargePercentage(&charge);
        return charge;
#endif
    }

    inline bool Charging() {
#ifdef SL_QEXT
        return qext_battery_charging() > 0;
#else
        PsmChargerType charger = PsmChargerType_Unconnected;
        return R_SUCCEEDED(psmGetChargerType(&charger)) && charger != PsmChargerType_Unconnected;
#endif
    }

    inline bool ListUsers(AccountUid *uids, s32 cap, s32 *count) {
#ifdef SL_QEXT
        *count = qext_account_list(uids, cap);
        if (*count > cap) *count = cap;
        return *count >= 0;
#else
        return R_SUCCEEDED(accountListAllUsers(uids, cap, count));
#endif
    }

    inline bool WirelessOn(bool &on) {
#ifdef SL_QEXT
        const int airplane = qext_airplane_mode();
        if (airplane < 0) return false;
        on = airplane == 0;
        return true;
#else
        return R_SUCCEEDED(nifmIsWirelessCommunicationEnabled(&on));
#endif
    }

}
