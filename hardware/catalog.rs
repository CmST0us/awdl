#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Hardware {
    Be200,
    Rtl8852be,
}

impl Hardware {
    pub const ALL: &[Self] = &[Self::Be200, Self::Rtl8852be];

    pub fn name(self) -> &'static str {
        match self {
            Self::Be200 => "be200",
            Self::Rtl8852be => "rtl8852be",
        }
    }

    pub fn from_name(name: &str) -> Option<Self> {
        Self::ALL
            .iter()
            .copied()
            .find(|hardware| hardware.name() == name)
    }

    pub fn detect(vendor: &str, device: &str) -> Option<Self> {
        match (vendor, device) {
            ("0x8086", "0x272b") => Some(Self::Be200),
            ("0x10ec", "0xb852") => Some(Self::Rtl8852be),
            _ => None,
        }
    }
}
