#pragma once
#include <sl/smi/Protocol.hpp>

// Convenience wrappers - sMenu side sends commands to sSystem

namespace sl::menu::smi {

    inline Result LaunchApp(u64 app_id) {
        sl::smi::PayloadAppLaunch p { app_id };
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::LaunchApplication, p);
    }

    inline Result ResumeApp() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::ResumeApplication);
    }

    inline Result TerminateApp() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::TerminateApplication);
    }

    inline Result SetUser(AccountUid uid) {
        sl::smi::PayloadSetUser p { uid };
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::SetSelectedUser, p);
    }

    inline Result OpenAlbum() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenAlbum);
    }

    inline Result OpenNetConnect() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenNetConnect);
    }

    inline Result OpenSystemSettings() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenSystemSettings);
    }

    inline Result OpenMiiEdit() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenMiiEdit);
    }

    inline Result OpenWebBrowser() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenWebBrowser);
    }

    inline Result OpenControllers() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenControllers);
    }

    inline Result OpenHomebrewMenu() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenHomebrewMenu);
    }

    // Launch a specific .nro (as an applet, via hbloader).
    inline Result OpenHomebrew(const char *nro_path, const char *argv = nullptr) {
        sl::smi::PayloadHomebrew p {};
        strncpy(p.nro_path, nro_path ? nro_path : "", sizeof(p.nro_path) - 1);
        strncpy(p.argv, argv ? argv : (nro_path ? nro_path : ""), sizeof(p.argv) - 1);
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenHomebrew, p);
    }

    // Launch a .nro as an application in a donor game's slot (full RAM / perms).
    inline Result LaunchHomebrewApp(u64 donor_id, const char *nro_path) {
        sl::smi::PayloadHomebrew p {};
        strncpy(p.nro_path, nro_path ? nro_path : "", sizeof(p.nro_path) - 1);
        strncpy(p.argv, nro_path ? nro_path : "", sizeof(p.argv) - 1);
        p.donor_id = donor_id;
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::LaunchHomebrewApplication, p);
    }

    inline Result OpenUserPage() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenUserPage);
    }

    inline Result OpenPowerMenu() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::OpenPowerMenu);
    }

    inline Result RestartMenu() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::RestartMenu);
    }

    inline Result ReloadAppList() {
        return sl::smi::SendMenuCommand(sl::smi::SystemMessage::ReloadAppList);
    }

} // namespace sl::menu::smi
