#!/bin/bash

export CC="/home/zbtrs/ospp/rt-thread/bsp/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-gcc"
export GCC="/home/zbtrs/ospp/rt-thread/bsp/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-gcc"
export AAR="/home/zbtrs/ospp/rt-thread/bsp/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-ar"
export AR="/home/zbtrs/ospp/rt-thread/bsp/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-ar"
export STRIP="/home/zbtrs/ospp/rt-thread/bsp/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-strip"
export RANLIB="/home/zbtrs/ospp/rt-thread/bsp/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-ranlib"
export CFLAGS="-mcmodel=medany -g"
export LDFLAGS="-Wl,--start-group -Wl,-whole-archive -L/home/zbtrs/ospp/userapps/sdk/rt-thread/lib/risc-v/rv64gc/ -lrtthread -Wl,-no-whole-archive -Wl,--end-group -n --static -T /home/zbtrs/ospp/userapps/tools/ldscripts/riscv64/link.lds -Wl,-Map=output.map"
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/zbtrs/ospp/rt-thread/bsp/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/lib/gcc/riscv64-unknown-linux-musl/10.1.0
