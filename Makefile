IMGUI_DIR = third_party/imgui
IMGUI_SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
SOURCES = kcov-gui.cpp kcov-autorace.cpp
SOURCES += $(IMGUI_SOURCES)
IMGUI_OBJS = $(addsuffix .o, $(basename $(IMGUI_SOURCES)))

CXXFLAGS = -std=gnu++23 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
CXXFLAGS += -ggdb -Wall -Wformat -Wno-unused-function
LIBS = -ldw -lGL `pkg-config --static --libs glfw3` -lcapstone -lelf

CXXFLAGS += `pkg-config --cflags glfw3`
CFLAGS = -std=gnu23 -Wall -Wno-unused-function

.PHONY: all clean
all: kcov-gui kcov-autorace kcov-vsock-client kcov-terminal
clean:
	rm -f kcov-gui kcov-autorace kcov-vsock-client $(IMGUI_OBJS) testcase/*.so *.d

kcov-gui: kcov-gui.cpp $(IMGUI_OBJS) kcov-common.h common.h basic.h
	$(CXX) -o $@ $< $(IMGUI_OBJS) $(CXXFLAGS) $(LIBS)

kcov-autorace: kcov-autorace.cpp kcov-common.h runner-common.h basic.h
	$(CXX) -o $@ $< $(CXXFLAGS) $(LIBS)

kcov-vsock-client: kcov-vsock-client.c runner-common.h common.h basic.h
	$(CC) -o $@ $< $(CFLAGS)

kcov-terminal: kcov-terminal.cpp kcov-common.h runner-common.h basic.h
	$(CXX) -o $@ $< $(CXXFLAGS) $(LIBS)

testcase/%.so: testcase/%.c
	$(CC) -shared -o $@ $< -Wall -fPIC
