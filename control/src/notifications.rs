// SPDX-License-Identifier: MIT

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NotificationEvent {
    ServiceLost,
    ServiceRecovered,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct NotificationState {
    observed_ready: bool,
    absent_since_ms: Option<u64>,
    outage_notified: bool,
}

impl NotificationState {
    pub fn update(
        &mut self,
        compatible_ready: bool,
        owner_present: bool,
        now_ms: u64,
    ) -> Option<NotificationEvent> {
        if compatible_ready {
            self.observed_ready = true;
            self.absent_since_ms = None;
            if self.outage_notified {
                self.outage_notified = false;
                return Some(NotificationEvent::ServiceRecovered);
            }
            return None;
        }

        if owner_present || !self.observed_ready || self.outage_notified {
            self.absent_since_ms = None;
            return None;
        }

        let absent_since = *self.absent_since_ms.get_or_insert(now_ms);
        if now_ms.saturating_sub(absent_since) >= 5_000 {
            self.outage_notified = true;
            return Some(NotificationEvent::ServiceLost);
        }
        None
    }

    pub fn next_wakeup_ms(&self) -> Option<u64> {
        (!self.outage_notified)
            .then(|| {
                self.absent_since_ms
                    .map(|start| start.saturating_add(5_000))
            })
            .flatten()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn initial_absence_never_notifies() {
        let mut state = NotificationState::default();
        assert_eq!(state.update(false, false, 0), None);
        assert_eq!(state.update(false, false, 50_000), None);
    }

    #[test]
    fn persistent_post_ready_loss_and_recovery_notify_once() {
        let mut state = NotificationState::default();
        assert_eq!(state.update(true, true, 0), None);
        assert_eq!(state.update(false, false, 10), None);
        assert_eq!(state.update(false, false, 5_009), None);
        assert_eq!(
            state.update(false, false, 5_010),
            Some(NotificationEvent::ServiceLost)
        );
        assert_eq!(state.update(false, false, 10_000), None);
        assert_eq!(
            state.update(true, true, 10_001),
            Some(NotificationEvent::ServiceRecovered)
        );
        assert_eq!(state.update(true, true, 10_002), None);
    }

    #[test]
    fn transient_health_does_not_start_outage_timer() {
        let mut state = NotificationState::default();
        state.update(true, true, 0);
        assert_eq!(state.update(false, true, 1), None);
        assert_eq!(state.next_wakeup_ms(), None);
    }
}
