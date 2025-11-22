import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
  alias(libs.plugins.android.application)
  alias(libs.plugins.kotlin.android)
}

android {
  namespace = "io.github.eirv.replacetext"
  compileSdk {
    version = release(36)
  }

  defaultConfig {
    applicationId = "io.github.eirv.replacetext"
    minSdk = 26
    targetSdk = 36
    versionCode = 1
    versionName = "1.0"
  }

  buildTypes {
    release {
      isMinifyEnabled = true
      proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
      signingConfig = signingConfigs.getByName("debug")
    }
  }

  flavorDimensions += "api"
  productFlavors {
    create("newapi") {
      dimension = "api"
      applicationIdSuffix = ".newapi"
      versionNameSuffix = "-newapi"
    }
    create("legacy") {
      dimension = "api"
      applicationIdSuffix = ".legacy"
      versionNameSuffix = "-legacy"
    }
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
}

dependencies {
  "newapiCompileOnly"(libs.xposed.newapi)
  "legacyCompileOnly"(libs.xposed.api)
}
