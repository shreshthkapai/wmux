#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum PresentationError {
    AlreadyInFlight,
    UnexpectedAck { expected: Option<u64>, actual: u64 },
    SequenceExhausted,
}

#[derive(Clone, Debug)]
pub(crate) struct PresentationGate {
    next_sequence: u64,
    in_flight: Option<u64>,
}

impl PresentationGate {
    pub(crate) fn new() -> Self {
        Self {
            next_sequence: 0,
            in_flight: None,
        }
    }

    #[cfg(test)]
    fn with_next_sequence(next_sequence: u64) -> Self {
        Self {
            next_sequence,
            in_flight: None,
        }
    }

    pub(crate) const fn ready(&self) -> bool {
        self.in_flight().is_none()
    }

    pub(crate) fn begin(&mut self) -> Result<u64, PresentationError> {
        if self.in_flight.is_some() {
            return Err(PresentationError::AlreadyInFlight);
        }
        let next_sequence = self
            .next_sequence
            .checked_add(1)
            .ok_or(PresentationError::SequenceExhausted)?;
        let sequence = self.next_sequence;
        self.next_sequence = next_sequence;
        self.in_flight = Some(sequence);
        Ok(sequence)
    }

    pub(crate) fn acknowledge(&mut self, sequence: u64) -> Result<(), PresentationError> {
        if self.in_flight != Some(sequence) {
            return Err(PresentationError::UnexpectedAck {
                expected: self.in_flight,
                actual: sequence,
            });
        }
        self.in_flight = None;
        Ok(())
    }

    pub(crate) const fn in_flight(&self) -> Option<u64> {
        self.in_flight
    }

    pub(crate) fn cancel(&mut self, sequence: u64) -> bool {
        if self.in_flight != Some(sequence) {
            return false;
        }
        self.in_flight = None;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::{PresentationError, PresentationGate};

    #[test]
    fn allows_only_one_frame_until_the_matching_acknowledgement() {
        let mut gate = PresentationGate::new();

        assert!(gate.ready());
        assert_eq!(gate.begin().unwrap(), 0);
        assert_eq!(gate.in_flight(), Some(0));
        assert!(!gate.ready());
        assert_eq!(gate.begin(), Err(PresentationError::AlreadyInFlight));

        gate.acknowledge(0).unwrap();

        assert!(gate.ready());
        assert_eq!(gate.in_flight(), None);
        assert_eq!(gate.begin().unwrap(), 1);
    }

    #[test]
    fn unexpected_acknowledgement_leaves_the_expected_sequence_in_flight() {
        let mut gate = PresentationGate::new();
        assert_eq!(gate.begin().unwrap(), 0);

        assert_eq!(
            gate.acknowledge(1),
            Err(PresentationError::UnexpectedAck {
                expected: Some(0),
                actual: 1,
            })
        );
        assert_eq!(gate.in_flight(), Some(0));

        gate.acknowledge(0).unwrap();
        assert_eq!(
            gate.acknowledge(0),
            Err(PresentationError::UnexpectedAck {
                expected: None,
                actual: 0,
            })
        );
    }

    #[test]
    fn exhausted_sequence_space_never_wraps() {
        let mut gate = PresentationGate::with_next_sequence(u64::MAX);

        assert_eq!(gate.begin(), Err(PresentationError::SequenceExhausted));
        assert!(gate.ready());
        assert_eq!(gate.in_flight(), None);
    }

    #[test]
    fn cancellation_only_clears_the_matching_reservation() {
        let mut gate = PresentationGate::new();
        let sequence = gate.begin().unwrap();

        assert!(!gate.cancel(sequence + 1));
        assert_eq!(gate.in_flight(), Some(sequence));
        assert!(gate.cancel(sequence));
        assert!(gate.ready());
        assert_eq!(gate.begin().unwrap(), sequence + 1);
    }
}
