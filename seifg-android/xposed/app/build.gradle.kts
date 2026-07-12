plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
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

    buildFeatures {
        compose = true
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

    implementation(platform("androidx.compose:compose-bom:2024.12.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")

    implementation("com.github.topjohnwu.libsu:core:6.0.0")
}
