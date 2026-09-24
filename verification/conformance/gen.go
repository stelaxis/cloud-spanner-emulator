package main

import (
	"math/rand/v2"
)

// genConfig bounds the random schedules.
type genConfig struct {
	numKeys int // keys 0..numKeys-1
	minTxns int // concurrent transactions besides the setup one
	maxTxns int
	maxOps  int // data operations per RW transaction
}

var tables = []string{"A", "B"}

type generator struct {
	r   *rand.Rand
	cfg genConfig
}

func (g *generator) table() string {
	if g.r.IntN(4) == 0 {
		return "B"
	}
	return "A"
}

func (g *generator) key() int     { return g.r.IntN(g.cfg.numKeys) }
func (g *generator) val() int64   { return int64(g.r.IntN(100)) }
func (g *generator) point() *Keys { k := g.key(); return &Keys{Point: &k} }

func (g *generator) keys(allowAll bool) *Keys {
	switch n := g.r.IntN(10); {
	case n < 4:
		return g.point()
	case n < 8 || !allowAll:
		lo, hi := g.key(), g.key()
		if lo > hi {
			lo, hi = hi, lo
		}
		if lo == hi {
			// An empty range such as (k,k) crashes upstream 1.5.58 once the
			// transaction has buffered writes in the table; see
			// testdata/upstream_crash_empty_open_range.jsonl.
			return &Keys{Lo: lo, Hi: hi, LoIncl: true, HiIncl: true}
		}
		return &Keys{Lo: lo, Hi: hi, LoIncl: g.r.IntN(3) > 0, HiIncl: g.r.IntN(2) > 0}
	default:
		return &Keys{All: true}
	}
}

func (g *generator) dataOp(txn int) Step {
	switch g.r.IntN(7) {
	case 0, 1:
		return Step{Txn: txn, Op: "read", Table: g.table(), Keys: g.keys(true)}
	case 2:
		return Step{Txn: txn, Op: "sql", Table: g.table(), Keys: g.keys(true)}
	case 3:
		return Step{Txn: txn, Op: "dml_insert", Table: g.table(), Key: g.key(), Val: g.val()}
	case 4, 5:
		return Step{Txn: txn, Op: "dml_update", Table: g.table(), Keys: g.keys(false), Val: g.val()}
	default:
		return Step{Txn: txn, Op: "dml_delete", Table: g.table(), Keys: g.keys(false)}
	}
}

func (g *generator) mutation() Mut {
	kinds := []string{"insert", "update", "upsert", "delete"}
	return Mut{Kind: kinds[g.r.IntN(len(kinds))], Table: g.table(), Key: g.key(), Val: g.val()}
}

func (g *generator) rwProgram(txn int) []Step {
	steps := []Step{{Txn: txn, Op: "begin_rw"}}
	for range g.r.IntN(g.cfg.maxOps + 1) {
		steps = append(steps, g.dataOp(txn))
	}
	if g.r.IntN(10) == 0 {
		return append(steps, Step{Txn: txn, Op: "rollback"})
	}
	var muts []Mut
	for range g.r.IntN(3) {
		muts = append(muts, g.mutation())
	}
	return append(steps, Step{Txn: txn, Op: "commit", Muts: muts})
}

func (g *generator) roProgram(txn int) []Step {
	begin := Step{Txn: txn, Op: "begin_ro"}
	if g.r.IntN(5) < 2 {
		at := 1 + g.r.IntN(3)
		begin.At = &at
	}
	steps := []Step{begin}
	for range 1 + g.r.IntN(3) {
		op := "read"
		if g.r.IntN(3) == 0 {
			op = "sql"
		}
		steps = append(steps, Step{Txn: txn, Op: op, Table: g.table(), Keys: g.keys(true)})
	}
	return steps
}

// schedule returns a setup transaction (txn 0) followed by a random
// interleaving of 2-4 transactions' programs.
func (g *generator) schedule() []Step {
	var setup []Mut
	for _, t := range tables {
		for k := range g.cfg.numKeys {
			if g.r.IntN(2) == 0 {
				setup = append(setup, Mut{Kind: "insert", Table: t, Key: k, Val: g.val()})
			}
		}
	}
	sched := []Step{{Txn: 0, Op: "begin_rw"}, {Txn: 0, Op: "commit", Muts: setup}}

	n := g.cfg.minTxns + g.r.IntN(g.cfg.maxTxns-g.cfg.minTxns+1)
	progs := make([][]Step, n)
	for i := range progs {
		if g.r.IntN(5) == 0 {
			progs[i] = g.roProgram(i + 1)
		} else {
			progs[i] = g.rwProgram(i + 1)
		}
	}
	for {
		var live []int
		for i, p := range progs {
			if len(p) > 0 {
				live = append(live, i)
			}
		}
		if len(live) == 0 {
			return sched
		}
		i := live[g.r.IntN(len(live))]
		sched = append(sched, progs[i][0])
		progs[i] = progs[i][1:]
	}
}
