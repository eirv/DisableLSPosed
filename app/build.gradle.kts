plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "io.github.eirv.disablelsposed"
    compileSdk = 35
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "io.github.eirv.disablelsposed"
        minSdk = 33
        targetSdk = 35
        versionCode = 2
        versionName = "1.1"

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++23"
                abiFilters("arm64-v8a")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
}
