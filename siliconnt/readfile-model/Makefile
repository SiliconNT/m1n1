# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model test for rust/src/readfile.rs: the real file, with the real rust/src/nvme.rs
# and rust/src/gpt.rs, compiled for the build machine and reading a disk that is an array.
#
#   make        build it and run it
#   make big    the two tests left out of that: a file the size of the boot disk, in clusters
#               of one sector and of 8. They fill 2 GB and 6 GB of memory

.PHONY: all big clean

all:
	cargo test --target-dir build

big:
	cargo test --release --target-dir build -- --ignored --nocapture

clean:
	rm -rf build
