# AudioServer through LibAudio. Needs SERENITY_SOURCE_DIR and
# SERENITY_BUILD_DIR from SerenityOS's env.sh; flags mirror its CMake build.

ifeq ($(strip $(SERENITY_SOURCE_DIR)),)
$(error SERENITY_SOURCE_DIR is not set; source SerenityOS's env.sh)
endif
SERENITY_BUILD_DIR ?= $(SERENITY_SOURCE_DIR)/Build/x86_64

TOY_AUDIO_SERENITY_CXXFLAGS := -std=c++26 -O2 -fPIC -fno-exceptions -fsized-deallocation \
                               -Wall -Wextra -Wno-literal-suffix \
                               -I$(SERENITY_SOURCE_DIR) \
                               -I$(SERENITY_SOURCE_DIR)/Userland/Libraries \
                               -I$(SERENITY_SOURCE_DIR)/Userland/Services \
                               -I$(SERENITY_SOURCE_DIR)/Userland \
                               -I$(SERENITY_BUILD_DIR) \
                               -I$(SERENITY_BUILD_DIR)/Userland/Services \
                               -I$(SERENITY_BUILD_DIR)/Userland/Libraries \
                               -I$(SERENITY_BUILD_DIR)/Userland
TOY_AUDIO_SERENITY_LIBS := -laudio -lcore -lcorebasic -lcoreminimal -lipc -lthreading
