// SPDX-License-Identifier: GPL-2.0-or-later
//! Host-side model test for rust/src/readfile.rs.
//!
//! The real readfile.rs, nvme.rs and gpt.rs are compiled for the build machine. What they
//! get from the rest of m1n1 is stood in for here: `println!`, `c_size_t`, and `nvme_read()`,
//! which reads from an array instead of the SSD. The array holds a GPT and a FAT32 volume
//! with 4096-byte sectors, made by the same fatfs the real code reads it with.
#![allow(dead_code)]

extern crate alloc;

pub(crate) use std::println;

#[allow(non_camel_case_types)]
pub(crate) type c_size_t = usize;

#[path = "../../../rust/src/gpt.rs"]
pub mod gpt;
#[path = "../../../rust/src/nvme.rs"]
pub mod nvme;
#[path = "../../../rust/src/readfile.rs"]
pub mod readfile;

#[cfg(test)]
mod tests {
    use crate::readfile::{read_file, rust_read_file, Error};
    use core::ffi::c_void;
    use fatfs::{FileSystem, FormatVolumeOptions, FsOptions, IoBase, Read, Seek, SeekFrom, Write};
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::cell::{Cell, RefCell};
    use std::ffi::CString;
    use uuid::Uuid;

    const SECTOR: usize = 4096;

    // ---- the allocator: remembers the largest block each thread asked for ----------------

    struct Watching;

    thread_local! {
        static LARGEST: Cell<usize> = const { Cell::new(0) };
    }

    unsafe impl GlobalAlloc for Watching {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            let _ = LARGEST.try_with(|l| l.set(l.get().max(layout.size())));
            unsafe { System.alloc(layout) }
        }
        // Without this the 300 MB of the disk would be written to as zeroes
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            let _ = LARGEST.try_with(|l| l.set(l.get().max(layout.size())));
            unsafe { System.alloc_zeroed(layout) }
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            unsafe { System.dealloc(ptr, layout) }
        }
    }

    #[global_allocator]
    static ALLOCATOR: Watching = Watching;

    // ---- the SSD: nvme_read() of src/nvme.c, reading an array ----------------------------

    #[derive(Default)]
    struct Ssd {
        data: Vec<u8>,
        reads: u64,
        // nvme_read() fails from this many reads on
        fail_after: Option<u64>,
    }

    // One per thread, and the test harness runs each test on a thread of its own
    thread_local! {
        static SSD: RefCell<Ssd> = RefCell::new(Ssd::default());
    }

    #[no_mangle]
    pub unsafe extern "C" fn nvme_read(nsid: u32, lba: u64, buffer: *mut c_void) -> bool {
        SSD.with(|ssd| {
            let mut ssd = ssd.borrow_mut();
            // src/nvme.c refuses a buffer that is not on a 4 KB boundary. The partition is
            // in namespace 1
            if buffer as usize % SECTOR != 0 || nsid != 1 {
                return false;
            }
            ssd.reads += 1;
            if ssd.fail_after.is_some_and(|n| ssd.reads > n) {
                return false;
            }
            let Some(off) = (lba as usize).checked_mul(SECTOR) else {
                return false;
            };
            if off + SECTOR > ssd.data.len() {
                return false;
            }
            unsafe {
                core::ptr::copy_nonoverlapping(
                    ssd.data.as_ptr().add(off),
                    buffer as *mut u8,
                    SECTOR,
                )
            };
            true
        })
    }

    fn reads() -> u64 {
        SSD.with(|ssd| ssd.borrow().reads)
    }

    // ---- making the disk -----------------------------------------------------------------

    // The partition with the files. FAT32 needs 65525 clusters at least
    const PART_UUID: &str = "8d7a2f3e-5b1c-4e6f-9a0b-c1d2e3f4a5b6";
    const PART_LBA: usize = 1024;
    const PART_SECTORS: usize = 76800;
    // A partition before it, which is not the one
    const OTHER_UUID: &str = "11111111-2222-4333-8444-555555555555";

    /// A piece of the array, for fatfs to format and write
    struct Volume<'a> {
        bytes: &'a mut [u8],
        pos: usize,
    }

    impl IoBase for Volume<'_> {
        type Error = ();
    }

    impl Read for Volume<'_> {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize, ()> {
            let n = buf.len().min(self.bytes.len() - self.pos);
            buf[..n].copy_from_slice(&self.bytes[self.pos..self.pos + n]);
            self.pos += n;
            Ok(n)
        }
    }

    impl Write for Volume<'_> {
        fn write(&mut self, buf: &[u8]) -> Result<usize, ()> {
            let n = buf.len().min(self.bytes.len() - self.pos);
            self.bytes[self.pos..self.pos + n].copy_from_slice(&buf[..n]);
            self.pos += n;
            Ok(n)
        }
        fn flush(&mut self) -> Result<(), ()> {
            Ok(())
        }
    }

    impl Seek for Volume<'_> {
        fn seek(&mut self, from: SeekFrom) -> Result<u64, ()> {
            let pos = match from {
                SeekFrom::Start(n) => n as i64,
                SeekFrom::End(n) => self.bytes.len() as i64 + n,
                SeekFrom::Current(n) => self.pos as i64 + n,
            };
            if pos < 0 || pos as usize > self.bytes.len() {
                return Err(());
            }
            self.pos = pos as usize;
            Ok(self.pos as u64)
        }
    }

    fn put_gpt_entry(disk: &mut [u8], index: usize, uuid: &str, first: usize, last: usize) {
        let e = &mut disk[2 * SECTOR + index * 128..][..128];
        // The type: any that is not all zeroes
        e[0..16].copy_from_slice(&[0xEB; 16]);
        e[16..32].copy_from_slice(&Uuid::parse_str(uuid).unwrap().to_bytes_le());
        e[32..40].copy_from_slice(&(first as u64).to_le_bytes());
        e[40..48].copy_from_slice(&(last as u64).to_le_bytes());
    }

    /// Puts a disk with these files in the SSD of this thread. The files are written `piece`
    /// bytes at a time, in turns: with a piece smaller than the files, their clusters end up
    /// between each other's.
    fn insert_disk(files: &[(&str, &[u8])], piece: usize) {
        insert_disk_of(PART_SECTORS, 1, files, piece)
    }

    fn insert_disk_of(
        part_sectors: usize,
        cluster_sectors: usize,
        files: &[(&str, &[u8])],
        piece: usize,
    ) {
        let mut disk = vec![0u8; (PART_LBA + part_sectors) * SECTOR];

        // The header in LBA 1, the entries from LBA 2. Entry 1 is left empty
        let header = &mut disk[SECTOR..2 * SECTOR];
        header[0..8].copy_from_slice(b"EFI PART");
        header[24..32].copy_from_slice(&1u64.to_le_bytes());
        header[72..80].copy_from_slice(&2u64.to_le_bytes());
        header[80..84].copy_from_slice(&128u32.to_le_bytes());
        header[84..88].copy_from_slice(&128u32.to_le_bytes());
        put_gpt_entry(&mut disk, 0, OTHER_UUID, 6, PART_LBA - 1);
        put_gpt_entry(
            &mut disk,
            2,
            PART_UUID,
            PART_LBA,
            PART_LBA + part_sectors - 1,
        );

        {
            let mut volume = Volume {
                bytes: &mut disk[PART_LBA * SECTOR..],
                pos: 0,
            };
            let options = FormatVolumeOptions::new()
                .bytes_per_sector(SECTOR as u16)
                .bytes_per_cluster((cluster_sectors * SECTOR) as u32)
                .fat_type(fatfs::FatType::Fat32);
            fatfs::format_volume(&mut volume, options).unwrap();
            volume.pos = 0;

            let fs = FileSystem::new(volume, FsOptions::new()).unwrap();
            assert_eq!(fs.fat_type(), fatfs::FatType::Fat32);
            let root = fs.root_dir();
            root.create_dir("siliconnt").unwrap();
            let mut open: Vec<_> = files
                .iter()
                .map(|(path, bytes)| (root.create_file(path).unwrap(), *bytes))
                .collect();
            while open.iter().any(|(_, left)| !left.is_empty()) {
                for (file, left) in open.iter_mut() {
                    let (now, later) = left.split_at(piece.min(left.len()));
                    file.write_all(now).unwrap();
                    *left = later;
                }
            }
            drop(open);
            drop(root);
            fs.unmount().unwrap();
        }

        SSD.with(|ssd| {
            *ssd.borrow_mut() = Ssd {
                data: disk,
                ..Ssd::default()
            }
        });
    }

    /// Bytes that say where in which file they are
    fn contents(len: usize, seed: u64) -> Vec<u8> {
        let mut x = seed | 1;
        (0..len)
            .map(|_| {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                (x >> 24) as u8
            })
            .collect()
    }

    const WHOLE: usize = usize::MAX;
    const UNTOUCHED: u8 = 0xA5;

    // ---- the tests -----------------------------------------------------------------------

    #[test]
    fn puts_the_file_at_the_destination_and_says_how_long_it_is() {
        // Not a whole number of sectors
        let firmware = contents(3 * 1024 * 1024 + 100, 1);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; firmware.len() + 2 * SECTOR];

        let size = read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest).unwrap();

        assert_eq!(size, firmware.len());
        assert!(dest[..size] == firmware[..]);
        assert!(dest[size..].iter().all(|&b| b == UNTOUCHED));
    }

    #[test]
    fn reads_files_whose_clusters_are_between_each_other() {
        let firmware = contents(1024 * 1024 + 7, 2);
        let bootdisk = contents(2 * 1024 * 1024 + 4095, 3);
        insert_disk(
            &[
                ("siliconnt/firmware.fd", &firmware),
                ("siliconnt/bootdisk.img", &bootdisk),
            ],
            3 * SECTOR,
        );
        let mut dest = vec![UNTOUCHED; 4 * 1024 * 1024];

        let size = read_file(PART_UUID, "siliconnt/bootdisk.img", &mut dest).unwrap();
        assert_eq!(size, bootdisk.len());
        assert!(dest[..size] == bootdisk[..]);

        let size = read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest).unwrap();
        assert_eq!(size, firmware.len());
        assert!(dest[..size] == firmware[..]);
    }

    #[test]
    fn a_file_as_long_as_the_destination_fits() {
        let firmware = contents(5 * SECTOR + 1, 4);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; firmware.len()];

        let size = read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest).unwrap();

        assert_eq!(size, firmware.len());
        assert!(dest == firmware);
    }

    #[test]
    fn a_file_longer_than_the_destination_is_refused_and_nothing_is_written() {
        let firmware = contents(5 * SECTOR + 1, 5);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; firmware.len() - 1];

        let result = read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest);

        assert!(matches!(result, Err(Error::TooLarge)), "{result:?}");
        assert!(dest.iter().all(|&b| b == UNTOUCHED));
    }

    #[test]
    fn an_empty_file_has_length_zero() {
        insert_disk(&[("siliconnt/firmware.fd", &[])], WHOLE);
        let mut dest = vec![UNTOUCHED; SECTOR];

        let size = read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest).unwrap();

        assert_eq!(size, 0);
        assert!(dest.iter().all(|&b| b == UNTOUCHED));
    }

    #[test]
    fn a_file_that_is_not_there_is_an_error() {
        let firmware = contents(SECTOR, 6);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; 2 * SECTOR];

        let result = read_file(PART_UUID, "siliconnt/bootdisk.img", &mut dest);

        assert!(
            matches!(result, Err(Error::FATError(fatfs::Error::NotFound))),
            "{result:?}"
        );
        assert!(dest.iter().all(|&b| b == UNTOUCHED));
    }

    #[test]
    fn a_partition_that_is_not_there_is_an_error() {
        let firmware = contents(SECTOR, 7);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; 2 * SECTOR];

        let result = read_file(
            "00000000-1111-4222-8333-444444444444",
            "siliconnt/firmware.fd",
            &mut dest,
        );

        assert!(
            matches!(result, Err(Error::PartitionNotFound)),
            "{result:?}"
        );
    }

    #[test]
    fn the_partition_is_found_by_its_own_uuid_not_by_its_place() {
        // The partition before ours holds no file system
        let firmware = contents(SECTOR, 8);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; 2 * SECTOR];

        let result = read_file(OTHER_UUID, "siliconnt/firmware.fd", &mut dest);

        assert!(matches!(result, Err(Error::FATError(_))), "{result:?}");
    }

    #[test]
    fn a_uuid_that_is_not_one_is_an_error_and_the_disk_is_not_read() {
        let firmware = contents(SECTOR, 9);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; 2 * SECTOR];

        let result = read_file("not-a-uuid", "siliconnt/firmware.fd", &mut dest);

        assert!(matches!(result, Err(Error::BadArgs)), "{result:?}");
        assert_eq!(reads(), 0);
    }

    #[test]
    fn a_read_the_disk_refuses_is_an_error() {
        let firmware = contents(64 * SECTOR, 10);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; firmware.len()];
        read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest).unwrap();
        let all = reads();

        // Again, with the disk failing 10 reads before the end: inside the file
        SSD.with(|ssd| {
            let mut ssd = ssd.borrow_mut();
            ssd.reads = 0;
            ssd.fail_after = Some(all - 10);
        });
        let result = read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest);

        assert!(
            matches!(result, Err(Error::FATError(fatfs::Error::Io(())))),
            "{result:?}"
        );
    }

    #[test]
    fn a_file_said_to_be_longer_than_its_clusters_ends_where_they_end() {
        let firmware = contents(4 * SECTOR, 14);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        // The length in the directory entry (offset 28 of 32) made 3 clusters more than the
        // file has
        SSD.with(|ssd| {
            let data = &mut ssd.borrow_mut().data;
            let name = b"FIRMWAREFD ";
            let entry = data.windows(name.len()).position(|w| w == name).unwrap();
            data[entry + 28..entry + 32].copy_from_slice(&(7 * SECTOR as u32).to_le_bytes());
        });
        let mut dest = vec![UNTOUCHED; 8 * SECTOR];

        // fatfs follows the clusters when it is asked where the file ends (File::seek()), so
        // the length that comes back is one there are clusters for, and the read ends
        let size = read_file(PART_UUID, "siliconnt/firmware.fd", &mut dest).unwrap();

        assert_eq!(size, firmware.len());
        assert!(dest[..size] == firmware[..]);
        assert!(dest[size..].iter().all(|&b| b == UNTOUCHED));
    }

    #[test]
    fn the_file_does_not_pass_through_the_heap() {
        let bootdisk = contents(8 * 1024 * 1024, 11);
        insert_disk(&[("siliconnt/bootdisk.img", &bootdisk)], WHOLE);
        let mut dest = vec![UNTOUCHED; bootdisk.len()];

        LARGEST.with(|l| l.set(0));
        let size = read_file(PART_UUID, "siliconnt/bootdisk.img", &mut dest).unwrap();
        let largest = LARGEST.with(|l| l.get());

        assert_eq!(size, bootdisk.len());
        // The sector buffer of nvme.rs is the largest thing there is a reason to allocate
        assert!(
            largest <= SECTOR,
            "a block of {largest} bytes was allocated"
        );
    }

    /// `make big`: it fills 2 GB of memory
    #[test]
    #[ignore]
    fn a_file_the_size_of_the_boot_disk() {
        // bootdisk.img as R13 to R21 loaded it, in a partition of 1 GB with the firmware
        // image before it
        let firmware = contents(31_457_280, 15);
        let bootdisk = contents(673_185_792, 16);
        insert_disk_of(
            262144,
            1,
            &[
                ("siliconnt/firmware.fd", &firmware),
                ("siliconnt/bootdisk.img", &bootdisk),
            ],
            WHOLE,
        );
        let mut dest = vec![UNTOUCHED; bootdisk.len() + SECTOR];

        let size = read_file(PART_UUID, "siliconnt/bootdisk.img", &mut dest).unwrap();

        assert_eq!(size, bootdisk.len());
        assert!(dest[..size] == bootdisk[..]);
        assert!(dest[size..].iter().all(|&b| b == UNTOUCHED));
        println!(
            "{} bytes, {} sectors, in {} calls of nvme_read() with clusters of {} bytes",
            size,
            size.div_ceil(SECTOR),
            reads(),
            SECTOR
        );
    }

    /// `make big`: it fills 6 GB of memory
    #[test]
    #[ignore]
    fn a_file_the_size_of_the_boot_disk_in_clusters_of_8_sectors() {
        // The same two files in a partition of 4 GiB whose clusters are 32 KiB, 8 sectors
        // each: what `newfs_msdos -F 32 -c 8` is asked for on the M4
        const CLUSTER_SECTORS: usize = 8;
        let firmware = contents(31_457_280, 17);
        let bootdisk = contents(673_185_792, 18);
        insert_disk_of(
            1_048_576,
            CLUSTER_SECTORS,
            &[
                ("siliconnt/firmware.fd", &firmware),
                ("siliconnt/bootdisk.img", &bootdisk),
            ],
            WHOLE,
        );
        let mut dest = vec![UNTOUCHED; bootdisk.len() + SECTOR];

        let size = read_file(PART_UUID, "siliconnt/bootdisk.img", &mut dest).unwrap();

        assert_eq!(size, bootdisk.len());
        assert!(dest[..size] == bootdisk[..]);
        assert!(dest[size..].iter().all(|&b| b == UNTOUCHED));
        println!(
            "{} bytes, {} sectors, in {} calls of nvme_read() with clusters of {} bytes",
            size,
            size.div_ceil(SECTOR),
            reads(),
            CLUSTER_SECTORS * SECTOR
        );
    }

    #[test]
    fn from_c_the_length_comes_back_through_a_pointer() {
        let firmware = contents(3 * SECTOR + 9, 12);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; 4 * SECTOR];
        let uuid = CString::new(PART_UUID).unwrap();
        let path = CString::new("siliconnt/firmware.fd").unwrap();
        let mut size = usize::MAX;

        let ret = unsafe {
            rust_read_file(
                uuid.as_ptr(),
                path.as_ptr(),
                dest.as_mut_ptr() as *mut c_void,
                dest.len(),
                &mut size,
            )
        };

        assert_eq!(ret, 0);
        assert_eq!(size, firmware.len());
        assert!(dest[..size] == firmware[..]);
        assert!(dest[size..].iter().all(|&b| b == UNTOUCHED));
    }

    #[test]
    fn from_c_an_error_is_minus_one() {
        let firmware = contents(3 * SECTOR + 9, 13);
        insert_disk(&[("siliconnt/firmware.fd", &firmware)], WHOLE);
        let mut dest = vec![UNTOUCHED; 2 * SECTOR];
        let uuid = CString::new(PART_UUID).unwrap();
        let path = CString::new("siliconnt/firmware.fd").unwrap();
        let mut size = usize::MAX;

        let ret = unsafe {
            rust_read_file(
                uuid.as_ptr(),
                path.as_ptr(),
                dest.as_mut_ptr() as *mut c_void,
                dest.len(),
                &mut size,
            )
        };

        assert_eq!(ret, -1);
        assert!(dest.iter().all(|&b| b == UNTOUCHED));
    }
}
