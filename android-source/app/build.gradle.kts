// AudioHub —— 采集端 + 播放端 双模式
//
// 依赖说明：
//   早期探针刻意零依赖以降低首次构建风险；现在要按 Material Design 3 做正式界面，
//   因此引入 AppCompat + Material Components。版本均为查询 Google Maven 得到的最新稳定版。
plugins {
    id("com.android.application")
}

android {
    namespace = "com.audiohub.probe"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.audiohub.probe"
        // Android 10 (API 29) 是 AudioPlaybackCapture 的最低要求
        minSdk = 29
        targetSdk = 36
        versionCode = 15
        versionName = "1.0"
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        abortOnError = false
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.8.0")
    implementation("com.google.android.material:material:1.14.0")
    implementation("androidx.constraintlayout:constraintlayout:2.2.2")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
}
