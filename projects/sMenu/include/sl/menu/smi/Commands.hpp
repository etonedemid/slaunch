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
