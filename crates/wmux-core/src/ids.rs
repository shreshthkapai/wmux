macro_rules! id_type {
    ($name:ident) => {
        #[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
        pub struct $name(u64);

        impl $name {
            pub const fn new(value: u64) -> Self {
                Self(value)
            }

            pub const fn raw(self) -> u64 {
                self.0
            }
        }
    };
}

id_type!(ClientId);
id_type!(SessionId);
id_type!(SessionGroupId);
id_type!(WinlinkId);
id_type!(WindowId);
id_type!(PaneId);
id_type!(TimerId);
