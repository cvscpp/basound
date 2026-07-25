CXX      = c++
FLTK_CFLAGS != fltk-config --cxxflags
FLTK_LIBS   != fltk-config --ldflags

CXXFLAGS = -std=c++14 -Wall -Wextra ${FLTK_CFLAGS}
LDFLAGS  = ${FLTK_LIBS}

TARGET = bsm_digi00x
SRCS   = main.cpp \
         digi00x_device.cpp \
         vu_meter.cpp \
         mixer_window.cpp
OBJS   = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -lm -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
