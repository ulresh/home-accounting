// AGP 9 включает встроенную поддержку Kotlin (KGP 2.2.10), поэтому отдельный
// плагин org.jetbrains.kotlin.android не применяется. Плагин Compose-компилятора
// должен совпадать по версии со встроенным Kotlin.
plugins {
    id("com.android.application") version "9.2.1" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.10" apply false
}
