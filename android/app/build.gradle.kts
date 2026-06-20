import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.github.ulresh.homeaccounting"
    compileSdk = 37
    buildToolsVersion = "37.0.0"   // использовать установленную версию (SDK-каталог только для чтения)

    defaultConfig {
        applicationId = "com.github.ulresh.homeaccounting"
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"
    }

    buildFeatures {
        compose = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1,versions/**}"
            excludes += "META-INF/INDEX.LIST"
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2026.05.01"))
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.0")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    debugImplementation("androidx.compose.ui:ui-tooling")

    // JSON: Jackson streaming — даёт инкрементный (потоковый) разбор по значениям.
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")

    implementation("androidx.camera:camera-camera2:1.3.4")
    implementation("androidx.camera:camera-lifecycle:1.3.4")
    implementation("androidx.camera:camera-view:1.3.4")

    implementation("com.google.zxing:core:3.5.3")

    implementation("org.bouncycastle:bcpkix-jdk18on:1.78.1")
    implementation("org.bouncycastle:bcprov-jdk18on:1.78.1")

    // Юнит-тесты модели и синхронизации (JVM, без эмулятора).
    testImplementation("junit:junit:4.13.2")
}

// Прокинуть переменные кросс-платформенной проверки в test-воркер.
// LC_ALL=C.UTF-8 — чтобы JVM кодировала ИМЕНА файлов в UTF-8 (как на реальном
// Android), иначе кириллический каталог базы «Основная» бьётся в "????????".
tasks.withType<Test>().configureEach {
    environment("LC_ALL", "C.UTF-8")
    environment("XC_MODE", System.getenv("XC_MODE") ?: "")
    environment("XC_DIR", System.getenv("XC_DIR") ?: "")
}
