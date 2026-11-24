//! Logic of the MaxPack payload that needs no PS5: it is tested on the PC (`cargo test -p maxpack-logic`).
pub mod cache;
pub mod dups;
pub mod fresh;
pub mod http;
pub mod images;
pub mod inst;
pub mod mounter;
pub mod p5z;
pub mod roots;
pub mod sony;
pub mod version;
