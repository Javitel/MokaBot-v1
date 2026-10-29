plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.mokabot.app"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.mokabot.app"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    // The brew and settings pages come from the firmware's data/ folder
    // (copied at build time), so the app always shows the same pages.
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/pages"))
}

val copyPages by tasks.registering(Copy::class) {
    from(rootProject.file("../data")) {
        include("brew.html", "settings.html")
    }
    into(layout.buildDirectory.dir("generated/pages/pages"))
}
tasks.named("preBuild") { dependsOn(copyPages) }

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")
    implementation("androidx.webkit:webkit:1.11.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
