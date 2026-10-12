// Kotlin
// 顶层构建脚本（android 独立工程）

// 和 IDF 分开构建
// 只能 2.0.21
plugins {
    id("com.android.application") version "8.7.0" apply false
    id("org.jetbrains.kotlin.android") version "2.0.21" apply false
}
