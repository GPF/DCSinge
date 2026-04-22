export EXCLUDE_FUNCS="singe_startup"

export KOS_CFLAGS="-O0 -fno-lto -m4-single -ml \
  -matomic-model=soft-gusa \
  -D__DREAMCAST__ -D_arch_dreamcast -D_arch_sub_pristine \
  -I/opt/toolchains/dc/kos/include \
  -I/opt/toolchains/dc/kos/kernel/arch/dreamcast/include \
  -I/opt/toolchains/dc/kos/addons/include/ \
  -I/opt/toolchains/dc/kos/../kos-ports/include \
  -g \
  -finstrument-functions \
  -finstrument-functions-exclude-function-list=${EXCLUDE_FUNCS} -finstrument-functions-exclude-file-list=/opt/toolchains/dc/kos/kernel,/opt/toolchains/dc/kos/libc"

export KOS_LDFLAGS="-fno-lto -m4-single -ml \
  -Wl,--gc-sections \
  -T/opt/toolchains/dc/kos/utils/ldscripts/shlelf.xc \
  -nostdlib \
  -L/opt/toolchains/dc/kos/lib/dreamcast \
  -L/opt/toolchains/dc/kos/addons/lib/dreamcast \
  -L/opt/toolchains/dc/kos/../kos-ports/lib"

python3 dctrace.py build/singe_dreamcast.elf

dot -Tpng graph.dot -o graph.png