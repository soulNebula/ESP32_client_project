# ProGuard 占位规则
# 打开混淆时留着这两条，免得回调类被裁掉
-keep class com.smarthome.ble.ble.** { *; }
-keep interface com.smarthome.ble.ble.** { *; }
