CXX      = c++
FLTK_CFLAGS != fltk-config --cxxflags
FLTK_LIBS   != fltk-config --ldflags

CXXFLAGS = -std=c++14 -Wall -Wextra ${FLTK_CFLAGS} \
           -I../../sys/dev/basound/dice
LDFLAGS  = ${FLTK_LIBS}

TARGET = bsm_pf2626
SRCS   = main.cpp \
         pf2626_device.cpp \
         mixer_window.cpp \
         vu_meter.cpp
OBJS   = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -lm -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
