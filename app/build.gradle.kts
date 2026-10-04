plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }

android {
    namespace = "dev.radek.conventor"
    compileSdk = 35
    ndkVersion = "27.2.12479018"
    defaultConfig {
        applicationId = "dev.radek.conventor"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { cppFlags += listOf("-std=c++17", "-Wall", "-Wextra") } }
    }
    externalNativeBuild { cmake { path = file("../native/CMakeLists.txt"); version = "3.22.1" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
    testOptions { unitTests.isIncludeAndroidResources = true }
    buildTypes { release { isMinifyEnabled = false } }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
}
