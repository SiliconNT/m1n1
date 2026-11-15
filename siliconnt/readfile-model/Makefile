# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model test for rust/src/readfile.rs: the real file, with the real rust/src/nvme.rs
# and rust/src/gpt.rs, compiled for the build machine and reading a disk that is an array.
#
#   make        build it and run it
#   make big    the one test left out of that: a file the size of the boot disk. It fills
#               2 GB of memory

.PHONY: all big clean

all:
	cargo test --target-dir build

big:
	cargo test --release --target-dir build -- --ignored --nocapture

clean:
	rm -rf build
