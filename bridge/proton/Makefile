CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -fPIC -Wall -Wextra
BRIDGE_SO = neural_amd_hip_bridge.so

all: $(BRIDGE_SO)

$(BRIDGE_SO): hip_bridge_linux.c hip_bridge_shared.h
	$(CC) $(CFLAGS) -shared -pthread -o $@ hip_bridge_linux.c -ldl

# PE trampoline (amdhip64_7.dll) is built on a Windows/mingw host:
#   clang -target x86_64-pc-windows-msvc -c amdhip64_7_trampoline.c -o trampoline.obj
#   lld-link /DLL /OUT:amdhip64_7.dll trampoline.obj
# or with mingw:
#   x86_64-w64-mingw32-gcc -shared -o amdhip64_7.dll amdhip64_7_trampoline.c
trampoline:
	x86_64-w64-mingw32-gcc -shared -O2 -o amdhip64_7.dll amdhip64_7_trampoline.c

clean:
	rm -f $(BRIDGE_SO) amdhip64_7.dll *.o

.PHONY: all trampoline clean
