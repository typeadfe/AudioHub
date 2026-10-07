package com.audiohub.probe;

import android.app.Application;
import androidx.appcompat.app.AppCompatDelegate;

public class AudioHubApp extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        AppCompatDelegate.setDefaultNightMode(Settings.darkMode(this)
                ? AppCompatDelegate.MODE_NIGHT_YES : AppCompatDelegate.MODE_NIGHT_NO);
    }
}
