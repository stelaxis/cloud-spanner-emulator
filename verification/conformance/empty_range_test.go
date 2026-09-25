package main

import (
	"math/rand/v2"
	"slices"
	"testing"
)

func TestEmptyRangeRegressionModel(t *testing.T) {
	sched, err := readSchedule("testdata/regression_empty_open_range.jsonl")
	if err != nil {
		t.Fatal(err)
	}
	rows := rowsOutcome([][2]int64{{2, 83}, {3, 83}})
	want := []Outcome{
		okOutcome(), committedOutcome(1), okOutcome(), countOutcome(2),
		rowsOutcome(nil), rowsOutcome(nil), rowsOutcome(nil), rows,
		committedOutcome(2), okOutcome(), rows,
	}
	for _, name := range []string{"upstream", "target"} {
		t.Run(name, func(t *testing.T) {
			m, err := startModel("../lean/.lake/build/bin/txnmodel", name, false)
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(m.close)
			out, err := m.run(sched)
			if err != nil {
				t.Fatal(err)
			}
			if !slices.Equal(out, want) {
				t.Fatalf("got %v; want %v", out, want)
			}
		})
	}
}

func TestGeneratorIncludesEmptyRanges(t *testing.T) {
	g := &generator{r: rand.New(rand.NewPCG(1, 0x5eed)), cfg: genConfig{numKeys: 6}}
	var openEqual, inverted, nonempty int
	for range 10000 {
		k := g.keys(true)
		if k.Point != nil || k.All {
			continue
		}
		switch {
		case k.Lo == k.Hi && !k.LoIncl && !k.HiIncl:
			openEqual++
		case k.Lo > k.Hi:
			inverted++
		case k.Lo < k.Hi:
			nonempty++
		}
	}
	if openEqual == 0 || inverted == 0 || nonempty == 0 {
		t.Fatalf("missing range coverage: open equal=%d inverted=%d nonempty=%d", openEqual, inverted, nonempty)
	}
}
