#include <sl/sys/ecs/ExternalContent.hpp>
#include <stratosphere.hpp>  // Atmosphere ldr:shel bindings

namespace sl::sys::ecs {

    static bool g_registered = false;
    static u64  g_program_id = 0x0100000000001010ULL; // eShop program ID (shop applet)

    Result RegisterMenu(const char *nso_path) {
        // Tell Atmosphere's loader to serve our NSO when the eShop slot is requested
        R_TRY(ldrShellAtmosphereRegisterExternalCode(g_program_id, nso_path));
        g_registered = true;
        return ResultSuccess();
    }

    void UnregisterMenu() {
        if (g_registered) {
            ldrShellAtmosphereUnregisterExternalCode(g_program_id);
            g_registered = false;
        }
    }

} // namespace sl::sys::ecs
