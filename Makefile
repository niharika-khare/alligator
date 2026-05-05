SRC_DIR := ./src
BUILD_DIR := ./build
BIN_DIR := ./bin
INC_ROOT_DIR := ./include


INC_DIR := $(shell find $(INC_ROOT_DIR) -type d)
INC_FLAG := $(addprefix -I,$(INC_DIR))


CFLAGS := $(INC_FLAG) -g

SRC := $(shell find $(SRC_DIR) -name '*.c')
DEPS := $(shell find $(INC_DIR) -name '*.h')
OBJ := $(patsubst $(BUILD_DIR)/%.o,$(SRC_DIR)/%.c,$(SRC))

.PHONY: help clean all build

all: build
	$(BIN_DIR)/alligator

help:
	@echo "Makefile targets: "
	@echo "  all.  	- 	Build and run all the changes (default)"
	@echo "  build 	- 	Build everything but do not run the application"
	@echo "  clean 	- 	clean all build targets and executables"
	@echo "  help  	- 	Display this help menu"

clean:
	rm -rf $(BIN_DIR) $(BUILD_DIR)


build: $(BIN_DIR) $(BUILD_DIR) alligator


$(BIN_DIR) $(BUILD_DIR):
	@mkdir -p $@


$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c  $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(CFLAGS)

alligator: $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $(BIN_DIR)/$@