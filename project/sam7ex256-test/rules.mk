# top level project rules for the sam7ex256-test project
#
LOCAL_DIR := $(GET_LOCAL_DIR)

TARGET := sam7ex256
MODULES += \
	app/tests

OBJS += \
	$(LOCAL_DIR)/init.o