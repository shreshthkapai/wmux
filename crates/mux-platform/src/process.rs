#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExitStatus {
    pub code: Option<i32>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TerminateMode {
    Graceful,
    Force,
}
