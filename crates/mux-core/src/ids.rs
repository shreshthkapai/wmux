macro_rules! id_type {
    ($name:ident) => {
        #[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
        pub struct $name(pub u32);

        impl $name {
            pub const NONE: Self = Self(0);

            pub const fn new(raw: u32) -> Self {
                Self(raw)
            }

            pub const fn raw(self) -> u32 {
                self.0
            }

            pub const fn is_none(self) -> bool {
                self.0 == 0
            }
        }
    };
}

id_type!(SessionId);
id_type!(WindowId);
id_type!(WinlinkId);
id_type!(PaneId);
id_type!(ClientId);
id_type!(JobId);
id_type!(PasteBufferId);
