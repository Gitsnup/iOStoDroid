plugins { id("com.android.application") }

android {
    // The runtime packager rewrites this template's package id and label the
    // same way it does for the other generated templates, so every template
    // shares the sentinel package id understood by BinaryXmlManifest and
    // ResourceTablePackagePatcher.
    namespace = "dev.iostodroid.placeholder"
    compileSdk = 35
    defaultConfig {
        applicationId = "dev.iostodroid.placeholder"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }
    testOptions { unitTests.isIncludeAndroidResources = true }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
}
