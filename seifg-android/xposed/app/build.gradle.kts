plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "net.seilent.seifg"
    compileSdk = 36

    defaultConfig {
        applicationId = "net.seilent.seifg"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-capture"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        debug {}
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    compileOnly("de.robv.android.xposed:api:82")
}
