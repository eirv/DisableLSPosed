import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
  alias(libs.plugins.android.application)
  alias(libs.plugins.kotlin.android)
}

android {
  namespace = "io.github.eirv.disablelsposed"
  ndkVersion = "29.0.14206865"

  compileSdk {
    version = release(36)
  }

  defaultConfig {
    applicationId = "io.github.eirv.disablelsposed"
    minSdk = 33
    targetSdk = 36
    versionCode = 10004
    versionName = "1.4"

    externalNativeBuild {
      cmake {
        arguments += "-DANDROID_STL=none"
        cppFlags += "-std=c++23"
        abiFilters("arm64-v8a", "armeabi-v7a", "x86", "x86_64", "riscv64")
      }
    }
  }

  buildTypes {
    release {
      isMinifyEnabled = true
      proguardFiles(
        getDefaultProguardFile("proguard-android-optimize.txt"),
        "proguard-rules.pro"
      )
      signingConfig = signingConfigs.getByName("debug")
    }
  }
  buildFeatures {
    prefab = true
  }
  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_11
    targetCompatibility = JavaVersion.VERSION_11
  }
  kotlin {
    target {
      compilerOptions {
        jvmTarget = JvmTarget.JVM_11
      }
    }
  }
  externalNativeBuild {
    cmake {
      path = file("src/main/cpp/CMakeLists.txt")
      version = "3.22.1"
    }
  }
  lint {
    checkReleaseBuilds = false
  }
}

dependencies {
  compileOnly(libs.cxx)
}
