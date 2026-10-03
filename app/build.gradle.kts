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
    lint {
        // dev.radek.generated.MainActivity is the entry activity of the APKs this
        // app *generates*; it is intentionally not declared in this manifest.
        disable += "Registered"
    }
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/runtimeAssets"))
}

/** The compiled converted-app runtime classes, wherever AGP placed them. */
fun runtimeClasses(root: File): Set<File> {
    val wanted = listOf("/dev/radek/generated/", "/dev/radek/runtime/")
    if (!root.isDirectory) return emptySet()
    return root.walkTopDown()
        .onEnter { !it.name.startsWith("tmp") && !it.name.startsWith(".") }
        .filter { file ->
            file.isFile && file.extension == "class" &&
                wanted.any { prefix -> file.path.replace('\\', '/').contains(prefix) }
        }
        .toSet()
}

/**
 * Packages the converted-app runtime (`dev.radek.generated`) as `assets/runtime.dex`
 * so a generated APK carries a real, minimal DEX instead of this application's own.
 *
 * The task never fails the build: when `d8` or the compiled classes are missing
 * (a stripped checkout, a host without build-tools) the converter falls back to
 * its own `classes.dex`, which contains the same runtime classes.
 */

tasks.register("bundleRuntimeDex") {
    group = "build"
    description = "Compiles the converted-app runtime into assets/runtime.dex"
    dependsOn("compileDebugJavaWithJavac")
    val assetsDir = layout.buildDirectory.dir("generated/runtimeAssets/assets")
    // AGP's javac output path is version dependent, so the runtime classes are
    // located by walking the build directory instead of a hard-coded path.
    val buildDir = layout.buildDirectory
    // The classes directory does not exist until javac has run, so it is declared
    // as an optional file collection (Gradle rejects a missing inputs.dir).
    inputs.files(runtimeClasses(buildDir.get().asFile)).optional(true)
    // Deliberately no outputs.dir: the generated directory is a *source* of the
    // main asset set, and declaring a producer would make every asset/lint task
    // demand an implicit dependency on this one. Ordering is wired explicitly
    // below for the merge tasks that actually build the APK.
    doLast {
        val output = assetsDir.get().asFile
        try {
            val classes = runtimeClasses(buildDir.get().asFile)
            if (classes.isEmpty()) {
                logger.warn("bundleRuntimeDex: no compiled runtime classes under $buildDir; generated APKs will reuse classes.dex")
                return@doLast
            }
            val sdk = android.sdkDirectory
            val androidJar = File(sdk, "platforms/android-${android.compileSdk}/android.jar")
            val d8 = File(sdk, "build-tools/${android.buildToolsVersion}/d8")
            if (!androidJar.isFile || !d8.exists()) {
                logger.warn("bundleRuntimeDex: missing " + (if (!androidJar.isFile) androidJar else d8))
                return@doLast
            }
            val staging = File(layout.buildDirectory.get().asFile, "runtime-dex")
            staging.deleteRecursively()
            staging.mkdirs()
            project.exec {
                commandLine = listOf(d8.absolutePath, "--release", "--min-api", "26",
                    "--lib", androidJar.absolutePath, "--output", staging.absolutePath) +
                    classes.map { it.absolutePath }
            }
            output.mkdirs()
            File(staging, "classes.dex").copyTo(File(output, "runtime.dex"), overwrite = true)
            logger.lifecycle("bundleRuntimeDex: wrote assets/runtime.dex (" + File(output, "runtime.dex").length() + " bytes)")
        } catch (error: Exception) {
            logger.warn("bundleRuntimeDex skipped: " + error.message)
        }
    }
}

tasks.matching { it.name == "mergeDebugAssets" || it.name == "mergeReleaseAssets" }.configureEach {
    dependsOn("bundleRuntimeDex")
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
}
