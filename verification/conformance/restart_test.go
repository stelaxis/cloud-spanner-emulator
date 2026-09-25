package main

import (
	"slices"
	"testing"
)

func restartFixture(durable, empty bool) []Step {
	muts := []Mut{{Kind: "upsert", Table: "A", Key: 0, Val: 70}, {Kind: "upsert", Table: "B", Key: 0, Val: 90}}
	if empty {
		muts = nil
	}
	return []Step{
		{Txn: 0, Op: "begin_rw"},
		{Txn: 0, Op: "commit", Muts: []Mut{{Kind: "upsert", Table: "A", Key: 0, Val: 7}, {Kind: "upsert", Table: "B", Key: 0, Val: 9}}},
		{Txn: 2, Op: "begin_rw"},
		{Txn: 2, Op: "crash", DuringCommit: true, Durable: durable, Muts: muts},
		{Op: "restart"},
		{Txn: 3, Op: "begin_ro"},
		{Txn: 3, Op: "read", Table: "A", Keys: &Keys{All: true}},
		{Txn: 3, Op: "read", Table: "B", Keys: &Keys{All: true}},
		{Txn: 4, Op: "begin_rw"},
		{Txn: 4, Op: "commit"},
	}
}

func testModel(t *testing.T, name, mode string) *model {
	t.Helper()
	m, err := startModel("../lean/.lake/build/bin/txnmodel", name, false, mode)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(m.close)
	return m
}

func TestRestartModel(t *testing.T) {
	for _, name := range []string{"upstream", "target"} {
		for _, tc := range []struct {
			name, mode     string
			durable, empty bool
			a, b           Outcome
			commit         int
		}{
			{"empty_restart", "no-persistence", false, false, rowsOutcome(nil), rowsOutcome(nil), 1},
			{"lost_commit", "persistent", false, false, rowsOutcome([][2]int64{{0, 7}}), rowsOutcome([][2]int64{{0, 9}}), 2},
			{"durable_commit", "persistent", true, false, rowsOutcome([][2]int64{{0, 70}}), rowsOutcome([][2]int64{{0, 90}}), 2},
			{"empty_crash_commit", "persistent", true, true, rowsOutcome([][2]int64{{0, 7}}), rowsOutcome([][2]int64{{0, 9}}), 2},
		} {
			t.Run(name+"/"+tc.name, func(t *testing.T) {
				m := testModel(t, name, tc.mode)
				out, err := m.run(restartFixture(tc.durable, tc.empty))
				if err != nil {
					t.Fatal(err)
				}
				want := []Outcome{okOutcome(), committedOutcome(1), okOutcome(), okOutcome(), okOutcome(), okOutcome(), tc.a, tc.b, okOutcome(), committedOutcome(tc.commit)}
				if !slices.Equal(out, want) {
					t.Fatalf("got %v; want %v", out, want)
				}
			})
		}
	}
}

func TestPersistentOracleRejectsPartialCommit(t *testing.T) {
	m := testModel(t, "upstream", "persistent")
	lost, err := m.run(restartFixture(false, false))
	if err != nil {
		t.Fatal(err)
	}
	durable, err := m.run(restartFixture(true, false))
	if err != nil {
		t.Fatal(err)
	}
	partial := slices.Clone(lost)
	partial[6] = durable[6] // only A survived, B did not
	if slices.Equal(partial, lost) || slices.Equal(partial, durable) {
		t.Fatal("torn multi-table commit accepted")
	}
}

func TestRestartRejectsOldHistoryAndLosesOpenTransactions(t *testing.T) {
	for _, name := range []string{"upstream", "target"} {
		t.Run(name, func(t *testing.T) {
			m := testModel(t, name, "persistent")
			at := 1
			tooLate := 100
			sched := []Step{
				{Txn: 0, Op: "begin_rw"}, {Txn: 0, Op: "commit"},
				{Txn: 1, Op: "begin_rw"}, {Txn: 1, Op: "dml_insert", Table: "A", Key: 0, Val: 99},
				{Op: "crash"}, {Txn: 1, Op: "commit"}, {Op: "restart"},
				{Txn: 1, Op: "commit"}, {Txn: 2, Op: "begin_ro", At: &at},
				{Txn: 3, Op: "begin_ro"}, {Txn: 3, Op: "read", Table: "A", Keys: &Keys{All: true}},
				{Op: "crash"}, {Op: "restart"}, {Txn: 4, Op: "begin_ro"}, {Txn: 4, Op: "read", Table: "A", Keys: &Keys{All: true}},
				{Txn: 5, Op: "begin_ro", At: &tooLate},
			}
			out, err := m.run(sched)
			if err != nil {
				t.Fatal(err)
			}
			for _, i := range []int{5, 7, 8, 15} {
				if out[i] != errorOutcome("FAILED_PRECONDITION") {
					t.Fatalf("step %d: %s", i, out[i])
				}
			}
			for _, i := range []int{10, 14} {
				if out[i] != rowsOutcome(nil) {
					t.Fatalf("lost transaction became visible at %d: %s", i, out[i])
				}
			}
		})
	}
}

func TestTerminalCrashCommitIsExecuted(t *testing.T) {
	for _, name := range []string{"upstream", "target"} {
		for _, mode := range []string{"no-persistence", "persistent"} {
			t.Run(name+"/"+mode, func(t *testing.T) {
				m := testModel(t, name, mode)
				sched := restartFixture(false, false)
				sched[3].Terminal = true
				sched[3].Muts = []Mut{{Kind: "insert", Table: "A", Key: 0, Val: 70}}
				out, err := m.run(sched)
				if err != nil {
					t.Fatal(err)
				}
				if out[3] != errorOutcome("ALREADY_EXISTS") {
					t.Fatalf("terminal commit was not checked: %s", out[3])
				}
			})
		}
	}
}
