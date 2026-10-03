plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }

val gameStubAssets = layout.buildDirectory.dir("generated/gameStubAssets")

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
    sourceSets.getByName("debug").assets.srcDir(gameStubAssets)
    buildTypes { release { isMinifyEnabled = false } }
}

val gameStubApk = project(":game-stub").layout.buildDirectory.file("outputs/apk/debug/game-stub-debug.apk")
val debugSigningKey = file("${System.getProperty("user.home")}/.android/debug.keystore")
val embedGameStubAssets = tasks.register<Copy>("embedGameStubAssets") {
    dependsOn(":game-stub:assembleDebug")
    from(gameStubApk) {
        into("game-stub")
        rename { "game-stub.apk" }
    }
    from(debugSigningKey) {
        into("game-stub")
        rename { "debug.keystore" }
    }
    into(gameStubAssets)
    doLast {
        check(gameStubAssets.get().file("game-stub/game-stub.apk").asFile.isFile) { "game-stub APK was not embedded" }
        check(gameStubAssets.get().file("game-stub/debug.keystore").asFile.isFile) { "debug keystore required for local game-stub APK signing is missing" }
    }
}
tasks.named("preBuild").configure { dependsOn(embedGameStubAssets) }

dependencies {
    implementation("com.android.tools.build:apksig:8.7.3")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
}
