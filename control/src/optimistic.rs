// SPDX-License-Identifier: MIT

use crate::model::ConfirmedTuple;

pub const MIN_START_INTERVAL_MS: u64 = 200;
pub const FINAL_TIMEOUT_MS: u64 = 3_000;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RouteTuple {
    pub volume: u32,
    pub muted: bool,
}

impl From<ConfirmedTuple> for RouteTuple {
    fn from(value: ConfirmedTuple) -> Self {
        Self {
            volume: value.volume,
            muted: value.muted,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct CarriedGenerations {
    volume: Option<u64>,
    mute: Option<u64>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Pending {
    tuple: RouteTuple,
    carried: CarriedGenerations,
    gesture: u64,
    final_dispatch: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Submitted {
    id: u64,
    tuple: RouteTuple,
    carried: CarriedGenerations,
    gesture: u64,
    started_ms: u64,
    base_revision: u64,
    sync_in_flight: bool,
    sync_failed: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct FinalDispatch {
    submission_id: u64,
    carried: CarriedGenerations,
    base_revision: u64,
    started_ms: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Dispatch {
    pub id: u64,
    pub tuple: RouteTuple,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Failure {
    Dispatch,
    Timeout,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Update {
    pub dispatch: Option<Dispatch>,
    pub failure: Option<Failure>,
}

impl Update {
    fn none() -> Self {
        Self {
            dispatch: None,
            failure: None,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Scheduler {
    confirmed: ConfirmedTuple,
    confirmed_revision: u64,
    display: RouteTuple,
    next_generation: u64,
    dirty_volume: Option<u64>,
    dirty_mute: Option<u64>,
    next_gesture: u64,
    active_gesture: Option<u64>,
    next_submission: u64,
    last_start_ms: Option<u64>,
    pending: Option<Pending>,
    submitted: Option<Submitted>,
    final_dispatch: Option<FinalDispatch>,
}

impl Scheduler {
    pub fn new(confirmed: ConfirmedTuple, confirmed_revision: u64) -> Self {
        Self {
            confirmed,
            confirmed_revision,
            display: confirmed.into(),
            next_generation: 0,
            dirty_volume: None,
            dirty_mute: None,
            next_gesture: 0,
            active_gesture: None,
            next_submission: 0,
            last_start_ms: None,
            pending: None,
            submitted: None,
            final_dispatch: None,
        }
    }

    pub fn display(&self) -> RouteTuple {
        self.display
    }

    #[cfg(test)]
    pub fn has_local_intent(&self) -> bool {
        self.dirty_volume.is_some()
            || self.dirty_mute.is_some()
            || self.pending.is_some()
            || self.final_dispatch.is_some()
    }

    pub fn begin_gesture(&mut self) {
        if self.active_gesture.is_none() {
            self.next_gesture += 1;
            self.active_gesture = Some(self.next_gesture);
        }
    }

    pub fn edit_volume(&mut self, volume: u32, now_ms: u64) -> Update {
        self.begin_gesture();
        self.next_generation += 1;
        self.dirty_volume = Some(self.next_generation);
        self.display.volume = volume.min(100);
        self.queue(false);
        self.maybe_start(now_ms)
    }

    pub fn edit_mute(&mut self, muted: bool, now_ms: u64) -> Update {
        self.begin_gesture();
        self.next_generation += 1;
        self.dirty_mute = Some(self.next_generation);
        self.display.muted = muted;
        self.queue(false);
        self.maybe_start(now_ms)
    }

    pub fn end_gesture(&mut self, now_ms: u64) -> Update {
        let Some(gesture) = self.active_gesture.take() else {
            return self.check_timeout(now_ms);
        };
        let latest = self.rebased_tuple();
        let carried = self.carried();

        if let Some(pending) = self.pending.as_mut()
            && pending.gesture == gesture
            && pending.tuple == latest
            && pending.carried == carried
        {
            pending.final_dispatch = true;
            return self.maybe_start(now_ms);
        }

        if let Some(submitted) = self.submitted
            && submitted.gesture == gesture
            && submitted.tuple == latest
            && submitted.carried == carried
        {
            if submitted.sync_failed {
                self.retire(carried);
                return Update {
                    dispatch: None,
                    failure: Some(Failure::Dispatch),
                };
            }
            if self.confirmed_revision > submitted.base_revision {
                self.retire(carried);
                return self.maybe_start(now_ms);
            }
            self.final_dispatch = Some(FinalDispatch {
                submission_id: submitted.id,
                carried,
                base_revision: submitted.base_revision,
                started_ms: submitted.started_ms,
            });
            return self.check_timeout(now_ms);
        }

        self.pending = Some(Pending {
            tuple: latest,
            carried,
            gesture,
            final_dispatch: true,
        });
        self.maybe_start(now_ms)
    }

    pub fn sync_completed(&mut self, submission_id: u64, success: bool, now_ms: u64) -> Update {
        let Some(mut submitted) = self.submitted else {
            return Update::none();
        };
        if submitted.id != submission_id || !submitted.sync_in_flight {
            return Update::none();
        }
        submitted.sync_in_flight = false;
        submitted.sync_failed = !success;
        self.submitted = Some(submitted);

        let is_current_final = self
            .final_dispatch
            .is_some_and(|final_dispatch| final_dispatch.submission_id == submission_id);
        let mut failure = None;
        if !success {
            if is_current_final && self.pending.is_none() {
                if let Some(final_dispatch) = self.final_dispatch.take() {
                    self.retire(final_dispatch.carried);
                    failure = Some(Failure::Dispatch);
                }
            } else if is_current_final {
                self.final_dispatch = None;
            }
        }

        let mut update = self.maybe_start(now_ms);
        if update.failure.is_none() {
            update.failure = failure;
        }
        update
    }

    pub fn apply_confirmation(
        &mut self,
        tuple: ConfirmedTuple,
        revision: u64,
        now_ms: u64,
    ) -> Update {
        if revision <= self.confirmed_revision {
            return self.check_timeout(now_ms);
        }
        self.confirmed = tuple;
        self.confirmed_revision = revision;

        if self.dirty_volume.is_none() {
            self.display.volume = tuple.volume;
        }
        if self.dirty_mute.is_none() {
            self.display.muted = tuple.muted;
        }

        if let Some(final_dispatch) = self.final_dispatch
            && revision > final_dispatch.base_revision
        {
            self.final_dispatch = None;
            self.retire(final_dispatch.carried);
        }

        self.rebase_pending();
        self.maybe_start(now_ms)
    }

    pub fn tick(&mut self, now_ms: u64) -> Update {
        let timeout = self.check_timeout(now_ms);
        if timeout.failure.is_some() {
            return timeout;
        }
        self.maybe_start(now_ms)
    }

    pub fn next_wakeup_ms(&self, now_ms: u64) -> Option<u64> {
        let dispatch_due = self.pending.and_then(|_| {
            if self.sync_in_flight() {
                None
            } else {
                Some(
                    self.last_start_ms
                        .map_or(now_ms, |last| last.saturating_add(MIN_START_INTERVAL_MS)),
                )
            }
        });
        let timeout_due = self
            .final_dispatch
            .map(|final_dispatch| final_dispatch.started_ms.saturating_add(FINAL_TIMEOUT_MS));
        match (dispatch_due, timeout_due) {
            (Some(left), Some(right)) => Some(left.min(right)),
            (Some(value), None) | (None, Some(value)) => Some(value),
            (None, None) => None,
        }
    }

    pub fn invalidate(&mut self) {
        self.display = self.confirmed.into();
        self.dirty_volume = None;
        self.dirty_mute = None;
        self.active_gesture = None;
        self.pending = None;
        self.submitted = None;
        self.final_dispatch = None;
        self.last_start_ms = None;
    }

    fn carried(&self) -> CarriedGenerations {
        CarriedGenerations {
            volume: self.dirty_volume,
            mute: self.dirty_mute,
        }
    }

    fn queue(&mut self, final_dispatch: bool) {
        self.pending = Some(Pending {
            tuple: self.rebased_tuple(),
            carried: self.carried(),
            gesture: self.active_gesture.expect("gesture was started"),
            final_dispatch,
        });
    }

    fn sync_in_flight(&self) -> bool {
        self.submitted
            .is_some_and(|submitted| submitted.sync_in_flight)
    }

    fn maybe_start(&mut self, now_ms: u64) -> Update {
        if self.sync_in_flight()
            || self.pending.is_none()
            || self
                .last_start_ms
                .is_some_and(|last| now_ms < last.saturating_add(MIN_START_INTERVAL_MS))
        {
            return Update::none();
        }

        self.rebase_pending();
        let pending = self.pending.take().expect("checked above");
        self.next_submission += 1;
        let submitted = Submitted {
            id: self.next_submission,
            tuple: pending.tuple,
            carried: pending.carried,
            gesture: pending.gesture,
            started_ms: now_ms,
            base_revision: self.confirmed_revision,
            sync_in_flight: true,
            sync_failed: false,
        };
        self.submitted = Some(submitted);
        self.last_start_ms = Some(now_ms);
        if pending.final_dispatch {
            self.final_dispatch = Some(FinalDispatch {
                submission_id: submitted.id,
                carried: submitted.carried,
                base_revision: submitted.base_revision,
                started_ms: now_ms,
            });
        }
        Update {
            dispatch: Some(Dispatch {
                id: submitted.id,
                tuple: submitted.tuple,
            }),
            failure: None,
        }
    }

    fn check_timeout(&mut self, now_ms: u64) -> Update {
        let timed_out = self.final_dispatch.is_some_and(|final_dispatch| {
            now_ms >= final_dispatch.started_ms.saturating_add(FINAL_TIMEOUT_MS)
        });
        if !timed_out {
            return Update::none();
        }
        let final_dispatch = self.final_dispatch.take().expect("checked above");
        self.retire(final_dispatch.carried);
        Update {
            dispatch: None,
            failure: Some(Failure::Timeout),
        }
    }

    fn retire(&mut self, carried: CarriedGenerations) {
        if carried.volume.is_some() && self.dirty_volume == carried.volume {
            self.dirty_volume = None;
            self.display.volume = self.confirmed.volume;
        }
        if carried.mute.is_some() && self.dirty_mute == carried.mute {
            self.dirty_mute = None;
            self.display.muted = self.confirmed.muted;
        }
        self.rebase_pending();
    }

    fn rebased_tuple(&self) -> RouteTuple {
        RouteTuple {
            volume: if self.dirty_volume.is_some() {
                self.display.volume
            } else {
                self.confirmed.volume
            },
            muted: if self.dirty_mute.is_some() {
                self.display.muted
            } else {
                self.confirmed.muted
            },
        }
    }

    fn rebase_pending(&mut self) {
        let rebased = self.rebased_tuple();
        if let Some(pending) = self.pending.as_mut() {
            pending.tuple = rebased;
            pending.carried = CarriedGenerations {
                volume: self.dirty_volume,
                mute: self.dirty_mute,
            };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tuple(volume: u32, muted: bool) -> ConfirmedTuple {
        ConfirmedTuple { volume, muted }
    }

    #[test]
    fn coalesces_and_spaces_dispatch_starts() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        assert_eq!(
            scheduler.edit_volume(20, 0).dispatch.unwrap().tuple.volume,
            20
        );
        scheduler.edit_volume(30, 50);
        scheduler.edit_volume(40, 80);
        assert!(scheduler.sync_completed(1, true, 100).dispatch.is_none());
        assert_eq!(scheduler.tick(199).dispatch, None);
        assert_eq!(scheduler.tick(200).dispatch.unwrap().tuple.volume, 40);
    }

    #[test]
    fn final_dedupe_promotes_existing_submission() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        let first = scheduler.edit_volume(20, 0).dispatch.unwrap();
        assert!(scheduler.end_gesture(10).dispatch.is_none());
        scheduler.sync_completed(first.id, true, 20);
        assert_eq!(scheduler.next_wakeup_ms(20), Some(3_000));
    }

    #[test]
    fn confirmation_before_final_promotion_retires_promoted_generations() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        let first = scheduler.edit_volume(20, 0).dispatch.unwrap();
        scheduler.apply_confirmation(tuple(15, false), 2, 10);
        scheduler.end_gesture(20);
        scheduler.sync_completed(first.id, true, 30);
        assert_eq!(scheduler.display().volume, 15);
        assert!(!scheduler.has_local_intent());
    }

    #[test]
    fn failed_intermediate_promoted_to_final_fails_without_resend() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        let first = scheduler.edit_volume(20, 0).dispatch.unwrap();
        assert_eq!(scheduler.sync_completed(first.id, false, 10).failure, None);
        let update = scheduler.end_gesture(20);
        assert_eq!(update.dispatch, None);
        assert_eq!(update.failure, Some(Failure::Dispatch));
        assert_eq!(scheduler.display().volume, 10);
        assert_eq!(scheduler.next_wakeup_ms(20), None);
    }

    #[test]
    fn differing_confirmation_replaces_final_optimism() {
        let mut scheduler = Scheduler::new(tuple(10, false), 4);
        let dispatch = scheduler.edit_volume(60, 0).dispatch.unwrap();
        scheduler.end_gesture(5);
        scheduler.sync_completed(dispatch.id, true, 10);
        scheduler.apply_confirmation(tuple(44, true), 5, 20);
        assert_eq!(
            scheduler.display(),
            RouteTuple {
                volume: 44,
                muted: true
            }
        );
    }

    #[test]
    fn timeout_rolls_back_only_carried_fields() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        let dispatch = scheduler.edit_volume(20, 0).dispatch.unwrap();
        scheduler.end_gesture(1);
        scheduler.sync_completed(dispatch.id, true, 2);
        scheduler.begin_gesture();
        scheduler.edit_mute(true, 100);
        let update = scheduler.tick(3_000);
        assert_eq!(update.failure, Some(Failure::Timeout));
        assert_eq!(
            scheduler.display(),
            RouteTuple {
                volume: 10,
                muted: true
            }
        );
    }

    #[test]
    fn clean_fields_rebase_before_dispatch() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        let first = scheduler.edit_volume(20, 0).dispatch.unwrap();
        scheduler.apply_confirmation(tuple(12, true), 2, 50);
        scheduler.edit_volume(30, 60);
        scheduler.sync_completed(first.id, true, 100);
        let second = scheduler.tick(200).dispatch.unwrap();
        assert_eq!(
            second.tuple,
            RouteTuple {
                volume: 30,
                muted: true
            }
        );
    }

    #[test]
    fn muted_volume_and_mute_only_are_atomic_full_tuples() {
        let mut volume = Scheduler::new(tuple(25, true), 1);
        assert_eq!(
            volume.edit_volume(50, 0).dispatch.unwrap().tuple,
            RouteTuple {
                volume: 50,
                muted: true
            }
        );

        let mut mute = Scheduler::new(tuple(25, false), 1);
        mute.begin_gesture();
        mute.apply_confirmation(tuple(70, false), 2, 0);
        assert_eq!(
            mute.edit_mute(true, 1).dispatch.unwrap().tuple,
            RouteTuple {
                volume: 70,
                muted: true
            }
        );
    }

    #[test]
    fn edits_after_final_dispatch_survive_confirmation() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        let first = scheduler.edit_volume(20, 0).dispatch.unwrap();
        scheduler.end_gesture(1);
        scheduler.sync_completed(first.id, true, 2);
        scheduler.edit_volume(30, 20);
        scheduler.apply_confirmation(tuple(18, false), 2, 30);
        assert_eq!(scheduler.display().volume, 30);
    }

    #[test]
    fn invalidation_discards_all_local_state() {
        let mut scheduler = Scheduler::new(tuple(10, false), 1);
        scheduler.edit_volume(80, 0);
        scheduler.end_gesture(1);
        scheduler.invalidate();
        assert_eq!(
            scheduler.display(),
            RouteTuple {
                volume: 10,
                muted: false
            }
        );
        assert!(!scheduler.has_local_intent());
        assert_eq!(scheduler.next_wakeup_ms(1), None);
    }
}
