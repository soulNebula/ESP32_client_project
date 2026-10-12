# 工程总入口，只做三件事：指定 CMake 最低版本、加载 IDF 的 project.cmake、声明工程名
# 工程名决定最终固件名（build/smart_home_s3.bin）
# 各组件怎么注册（源文件 / 头文件目录 / 依赖）都在 components/<组件>/CMakeLists.txt 里

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(smart_home_s3)
