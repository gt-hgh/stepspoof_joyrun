LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE           := stepspoof
LOCAL_SRC_FILES        := main.cpp
LOCAL_LDLIBS           := -llog -landroid
LOCAL_CPPFLAGS         := -std=c++17 -O2 -fvisibility=hidden -fno-exceptions -Wall -Wextra
LOCAL_CFLAGS           := -O2 -fvisibility=hidden
LOCAL_LDFLAGS          := -Wl,--gc-sections -Wl,-s
include $(BUILD_SHARED_LIBRARY)
