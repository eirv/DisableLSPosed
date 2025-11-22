pluginManagement {
  repositories {
    if ("CN" == System.getProperty("user.country")) {
      maven("https://maven.aliyun.com/repository/public/")
      maven("https://maven.aliyun.com/repository/google/")
      maven("https://maven.aliyun.com/repository/gradle-plugin/")
    }
    google {
      content {
        includeGroupByRegex("com\\.android.*")
        includeGroupByRegex("com\\.google.*")
        includeGroupByRegex("androidx.*")
      }
    }
    mavenCentral()
    gradlePluginPortal()
  }
}
dependencyResolutionManagement {
  repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
  repositories {
    if ("CN" == System.getProperty("user.country")) {
      maven("https://maven.aliyun.com/repository/public/")
      maven("https://maven.aliyun.com/repository/google/")
    }
    google()
    mavenCentral()
  }
}

rootProject.name = "DisableLSPosed"
include(":app")
