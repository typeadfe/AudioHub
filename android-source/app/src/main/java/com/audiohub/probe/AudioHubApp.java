package com.audiohub.probe;

import android.app.Application;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.core.os.LocaleListCompat;

public class AudioHubApp extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        String language = Settings.language(this);
        AppCompatDelegate.setApplicationLocales("system".equals(language)
                ? LocaleListCompat.getEmptyLocaleList()
                : LocaleListCompat.forLanguageTags(language));
        AppCompatDelegate.setDefaultNightMode(Settings.darkMode(this)
                ? AppCompatDelegate.MODE_NIGHT_YES : AppCompatDelegate.MODE_NIGHT_NO);
    }
}
