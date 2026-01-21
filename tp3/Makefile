# Makefile for Truck Platooning Simulator
# Compiles follower and leader as separate executables

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread

# Source files for follower
FOLLOWER_SRCS = follower.c event.c tpnet.c emergency.c intruder.c
FOLLOWER_OBJS = $(FOLLOWER_SRCS:.c=.o)
FOLLOWER_EXEC = follower

# Source files for leader
LEADER_SRCS = leader.c
LEADER_OBJS = $(LEADER_SRCS:.c=.o)
LEADER_EXEC = leader

# Headers
HEADERS = truckplatoon.h event.h follower.h tpnet.h

# Default target
all: $(FOLLOWER_EXEC) $(LEADER_EXEC)

# Build follower executable
$(FOLLOWER_EXEC): $(FOLLOWER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ Follower executable created: $@"

# Build leader executable
$(LEADER_EXEC): $(LEADER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ Leader executable created: $@"

# Compile object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(FOLLOWER_OBJS) $(LEADER_OBJS) $(FOLLOWER_EXEC) $(LEADER_EXEC)
	@echo "✓ Clean complete"

# Run leader in background
run-leader: $(LEADER_EXEC)
	./$(LEADER_EXEC) &

# Run single follower instance (pass port as argument)
run-follower: $(FOLLOWER_EXEC)
	./$(FOLLOWER_EXEC) 5001

# Help
help:
	@echo "Truck Platooning Simulator - Makefile targets:"
	@echo "  make              - Build both follower and leader"
	@echo "  make follower     - Build only follower"
	@echo "  make leader       - Build only leader"
	@echo "  make clean        - Remove all build artifacts"
	@echo "  make run-leader   - Build and run leader (background)"
	@echo "  make run-follower - Build and run single follower (port 5001)"
	@echo ""
	@echo "Example: Run leader, then follower in separate terminals:"
	@echo "  Terminal 1: make run-leader"
	@echo "  Terminal 2: ./follower 5001"
	@echo "  Terminal 3: ./follower 5002"

# Phony targets
.PHONY: all clean run-leader run-follower help follower leader
