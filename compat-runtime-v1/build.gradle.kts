plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.radek.compat.runtime"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-Wall", "-Wextra")
                arguments += listOf(
                    "-DRADEK_BUILD_COMPAT_RUNTIME=ON",
                    "-DRADEK_FETCH_UNICORN=ON",
                )
                // Unicorn is a shared CMake dependency. Declare it as a build target too so AGP
                // packages libunicorn.so into the AAR; the generated game APK needs it beside
                // libcompat_runtime_v1.so at runtime.
                targets += listOf("compat_runtime_v1", "unicorn")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildTypes { release { isMinifyEnabled = false } }
}

dependencies { }
