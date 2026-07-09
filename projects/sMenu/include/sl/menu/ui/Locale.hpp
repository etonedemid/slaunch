#pragma once

// Lightweight localization for the menu UI. English is the built-in language:
// the source strings ARE the keys, so anything without a translation simply
// renders in English (guaranteed fallback). A translation is an optional file on
// the SD card - sdmc:/slaunch/lang/<code>.txt - with "English text=translation"
// lines. The console's system language selects which file to load.

namespace sl::menu::ui {

    // Detect the system language and load its locale file if one exists. Safe to
    // call once at startup (after setInitialize()).
    void LocaleInit();

    // Translate an English UI string to the active language, or return it
    // unchanged when there is no entry for it. The returned pointer is stable.
    const char *T(const char *english);

    // Active language code (e.g. "en", "fr", "ja"); "en" when none was loaded.
    const char *LocaleCode();

} // namespace sl::menu::ui
