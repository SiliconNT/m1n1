// SPDX-License-Identifier: GPL-2.0-or-later
#![deny(unsafe_op_in_unsafe_fn)]

//! Reads a file of a FAT partition of the internal SSD to where the caller wants it.
//!
//! chainload.rs reads a file into a `Vec` on the heap of m1n1, which a second stage m1n1 fits
//! in and a disk image of hundreds of megabytes does not. Here the file goes straight to its
//! destination; the only buffer in between is the one sector of nvme.rs.

use crate::c_size_t;
use crate::gpt;
use crate::nvme;
use crate::println;
use core::ffi::{c_char, c_int, c_void, CStr};
use fatfs::{FileSystem, FsOptions, Read, Seek, SeekFrom};
use uuid::Uuid;

#[derive(Debug)]
pub enum Error {
    FATError(fatfs::Error<nvme::Error>),
    GPTError(gpt::Error<nvme::Error>),
    BadArgs,
    PartitionNotFound,
    TooLarge,
}

impl From<fatfs::Error<nvme::Error>> for Error {
    fn from(err: fatfs::Error<nvme::Error>) -> Error {
        Error::FATError(err)
    }
}

impl From<gpt::Error<nvme::Error>> for Error {
    fn from(err: gpt::Error<nvme::Error>) -> Error {
        Error::GPTError(err)
    }
}

/// Reads the file `path` of the FAT partition whose own UUID in the GPT is `part_uuid` to the
/// start of `dest`, and returns its length. A file longer than `dest` is an error, and then
/// `dest` is not written to. After any other error part of the file may be in `dest`.
///
/// The NVMe controller has to be running: nvme_init() before, nvme_shutdown() after.
pub fn read_file(part_uuid: &str, path: &str, dest: &mut [u8]) -> Result<usize, Error> {
    let uuid = Uuid::parse_str(part_uuid).or(Err(Error::BadArgs))?;

    let part = {
        let storage = nvme::NVMEStorage::new(1, 0);
        let mut pt = gpt::GPT::new(storage)?;
        pt.find_by_partuuid(uuid)?.ok_or(Error::PartitionNotFound)?
    };

    let storage = nvme::NVMEStorage::new(1, part.get_starting_lba());
    let opts = FsOptions::new().update_accessed_date(false);
    let fs = FileSystem::new(storage, opts)?;
    let mut file = fs.root_dir().open_file(path)?;

    let size = file.seek(SeekFrom::End(0))? as usize;
    if size > dest.len() {
        return Err(Error::TooLarge);
    }
    file.seek(SeekFrom::Start(0))?;

    // The length is one there are clusters for: File::seek() of fatfs follows them to the end.
    // read_exact() is an error, where a loop around read() would not end, if it were not
    file.read_exact(&mut dest[..size])?;

    Ok(size)
}

/// read_file() for C. Returns 0 and the length of the file in `*size`, or -1.
///
/// # Safety
///
/// `part_uuid` and `path` are C strings. `dest` is `max_size` bytes of memory that nothing
/// else reads or writes during the call. `size` points to a `size_t`.
#[no_mangle]
pub unsafe extern "C" fn rust_read_file(
    part_uuid: *const c_char,
    path: *const c_char,
    dest: *mut c_void,
    max_size: c_size_t,
    size: *mut c_size_t,
) -> c_int {
    let part_uuid = unsafe { CStr::from_ptr(part_uuid) }.to_str();
    let path = unsafe { CStr::from_ptr(path) }.to_str();
    let (Ok(part_uuid), Ok(path)) = (part_uuid, path) else {
        println!("readfile: the partition or the path is not UTF-8");
        return -1;
    };
    let dest = unsafe { core::slice::from_raw_parts_mut(dest as *mut u8, max_size) };

    match read_file(part_uuid, path, dest) {
        Ok(n) => {
            unsafe { *size = n };
            0
        }
        Err(err) => {
            println!("readfile: {}: {:?}", path, err);
            -1
        }
    }
}
