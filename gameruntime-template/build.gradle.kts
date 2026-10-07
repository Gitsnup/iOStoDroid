plugins { id("com.android.application") }

android {
    // The runtime packager rewrites this template's package id and label the
    // same way it does for the other generated templates, so every template
    // shares the sentinel package id understood by BinaryXmlManifest and
    // ResourceTablePackagePatcher.
    namespace = "dev.radek.placeholder"
    compileSdk = 35
    defaultConfig {
        applicationId = "dev.radek.placeholder"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }
}
