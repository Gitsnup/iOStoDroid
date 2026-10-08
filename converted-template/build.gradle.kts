plugins { id("com.android.application") }

android {
    // The runtime packager rewrites this template's package id and label the
    // same way it does for the preview shell, so both templates share the
    // sentinel package id understood by BinaryXmlManifest/ResourceTablePackagePatcher.
    namespace = "dev.iostodroid.placeholder"
    compileSdk = 35
    defaultConfig {
        applicationId = "dev.iostodroid.placeholder"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }
}
