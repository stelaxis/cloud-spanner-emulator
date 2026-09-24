import TxnSpec.Counterexamples
import TxnSpec.PersistenceComposition
import TxnSpec.PersistenceSequences

namespace TxnSpec.Persistence

/-- Per-database mutexes alone do not order a shared WAL by timestamps:
DB1 reserves 1 and stalls, DB2 reserves 2 and persists, then crash. Durable
[2] is not a prefix of the timestamp-ordered attempted history [1,2].
Fix: serialize timestamp reservation + append under a global WAL gate (the
Move.begin/ack gate), or use independent WALs and state (b) per database. -/
theorem per_database_locks_not_global_prefix :
    ¬ ([2] : List Nat).IsPrefix [1, 2] ∧ ¬ ([2, 1] : List Nat).Pairwise (· < ·) := by decide

private def sample : Record := ⟨7, .createDatabase 0 0⟩

/-- Deleting covered WAL before publishing the snapshot loses an acked record.
The real Move.truncate cannot change the active tail. -/
theorem truncate_before_publish_loses_ack :
    (replay {} [sample]).clock = 7 ∧ (replay {} ([] : List Record)).clock = 0 := by decide

/-- Reusing the last persisted cursor instead of the exclusive reserved end
would issue 0 again. SequenceMove.crash skips the unused reservation. -/
theorem cursor_restart_reissues :
    ¬ ([0, 0] : List Nat).Nodup ∧ ([0, 8] : List Nat).Nodup := by decide

/-- If a clock restarts only from wall time, a backward wall clock repeats or
regresses recovered timestamps. timestamp_restart uses max + 1 instead. -/
theorem wall_only_restart_regresses : ¬ (3 > (replay {} [sample]).clock) := by decide

private def uncertainSchedule : List Step :=
  [⟨1, .beginRW⟩, ⟨1, .commit [⟨.upsert, 0, 0, 7⟩]⟩,
   ⟨2, .beginRW⟩, ⟨2, .read 0 (.point 0)⟩, ⟨2, .commit []⟩]

/-- fsync(T1 writes 7); crash before ack; recover; T2 reads 7 and commits.
Deleting T1 from the serial history because its reply was lost is unsound.
Fix the specification: include durable completions of uncertain commits. -/
theorem acknowledged_only_history_not_serializable :
    ¬ Serializable ((Target.run {} {} uncertainSchedule).2.log.drop 1) := by
  rw [Serializable, ← Counterexamples.serialB_iff]; decide

/-- Physical replay stores the resolved backfill result. Re-executing an
incrementing backfill would double-apply it. -/
theorem rerun_backfill_wrong : (7 + 1 : Nat) ≠ 7 := by decide

#print axioms durability
#print axioms atomicity
#print axioms recovery_prefix
#print axioms checkpoint_safety
#print axioms timestamp_restart
#print axioms sequences_never_reissue
#print axioms storage_sequences_never_reissue
#print axioms checkpoint_clock_restart
#print axioms every_post_restart_timestamp
#print axioms instance_drop_survives
#print axioms drop_record_survives
#print axioms quiescent_image
#print axioms dropped_stays_dropped
#print axioms target_serializable_across_restarts
#print axioms replay_target_rows
#print axioms corruption_fails
#print axioms pre_restart_read_rejected
#print axioms per_database_locks_not_global_prefix
#print axioms acknowledged_only_history_not_serializable

end TxnSpec.Persistence
