// Command conformance runs random multi-transaction schedules against a Cloud
// Spanner emulator and against the Lean model (`txnmodel`), and diffs every
// step's outcome. See verification/README.md.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math/rand/v2"
	"os"
	"slices"
	"sort"
	"strings"
)

func main() { os.Exit(run()) }

// run returns the exit code, so deferred cleanup (the started container)
// happens on every path.
func run() int {
	var (
		persistence = flag.String("persistence", "", "crash driver: no-persistence | persistent")
		checkpoint  = flag.String("checkpoint-command", "", "optional asynchronous checkpoint trigger inside owned container (native: run by sh with $EMULATOR_PID)")
		binary      = flag.String("emulator-binary", "", "crash driver: run this emulator_main natively instead of a container")
		port        = flag.Int("port", 19210, "crash driver: the native emulator's port")
	)
	var (
		seeds      = flag.Int("seeds", 2000, "number of random schedules")
		firstSeed  = flag.Uint64("first-seed", 1, "seed of the first schedule")
		modelName  = flag.String("model", "target", "model to compare against: target (this fork) | upstream (v1.5.58)")
		pushdown   = flag.Bool("pushdown", false, "target model: SQL predicates narrow the read set")
		mustMatch  = flag.Bool("must-match", true, "exit non-zero on any mismatch")
		modelPath  = flag.String("txnmodel", "../lean/.lake/build/bin/txnmodel", "path to the txnmodel executable")
		image      = flag.String("image", "", "emulator image started when SPANNER_EMULATOR_HOST is unset (default: "+forkImage+" for -model target, "+upstreamImage+" for -model upstream)")
		abortProb  = flag.Int("abort-probability", 0, "--abort_current_transaction_probability for the started emulator")
		numKeys    = flag.Int("keys", 6, "key space size per table")
		minTxns    = flag.Int("min-txns", 2, "minimum concurrent transactions per schedule")
		maxTxns    = flag.Int("max-txns", 4, "maximum concurrent transactions per schedule")
		maxOps     = flag.Int("max-ops", 4, "maximum data operations per RW transaction")
		noShrink   = flag.Bool("no-shrink", false, "report failing schedules without shrinking")
		dumpFailed = flag.String("dump", "", "write the first (shrunk) mismatching schedule as JSON lines to this file")
		replay     = flag.String("schedule", "", "run this schedule (JSON lines) once, print the step-by-step diff, and exit")
		printOnly  = flag.Bool("print", false, "print the schedule of -first-seed as JSON lines (txnmodel input) and exit")
	)
	flag.Parse()
	log.SetFlags(0)
	cfg := genConfig{numKeys: *numKeys, minTxns: *minTxns, maxTxns: *maxTxns, maxOps: *maxOps}
	if *printOnly {
		g := &generator{r: rand.New(rand.NewPCG(*firstSeed, 0x5eed)), cfg: cfg}
		var sched []Step
		if *persistence != "" {
			sched = g.crashSchedule()
		} else {
			sched = g.schedule()
		}
		for _, st := range sched {
			line, _ := json.Marshal(st)
			fmt.Println(string(line))
		}
		return 0
	}

	if *image == "" {
		*image = forkImage
		if *modelName == "upstream" {
			*image = upstreamImage
		}
	}
	if *persistence != "" {
		return runRestarts(*persistence, *image, *binary, *port, *checkpoint, *modelPath, *modelName, *pushdown, *mustMatch, *seeds, *firstSeed, cfg, *replay, *dumpFailed)
	}
	addr := os.Getenv("SPANNER_EMULATOR_HOST")
	if addr == "" {
		// Only the fork has --enable_query_key_pushdown; it must agree with the
		// model's read sets.
		var extra []string
		if *modelName == "target" {
			extra = append(extra, fmt.Sprintf("--enable_query_key_pushdown=%v", *pushdown))
		}
		a, stop, err := startDocker(*image, *abortProb, extra...)
		if err != nil {
			log.Print(err)
			return 2
		}
		defer stop()
		addr = a
		log.Printf("started %s at %s (abort probability %d)", *image, addr, *abortProb)
	}
	emu, err := dialEmulator(addr)
	if err != nil {
		log.Print(err)
		return 2
	}
	ctx := context.Background()
	if err := emu.setup(ctx); err != nil {
		log.Print(err)
		return 2
	}
	mdl, err := startModel(*modelPath, *modelName, *pushdown)
	if err != nil {
		log.Print(err)
		return 2
	}
	defer mdl.close()

	h := &harness{ctx: ctx, emu: emu, mdl: mdl}
	if *replay != "" {
		sched, err := readSchedule(*replay)
		if err != nil {
			log.Print(err)
			return 2
		}
		emuOut, mdlOut, err := h.run(sched)
		if err != nil {
			log.Print(err)
			return 2
		}
		d := &divergence{sched: sched, emu: emuOut, model: mdlOut}
		fmt.Print(d.render(sched))
		if !slices.Equal(emuOut, mdlOut) && *mustMatch {
			return 1
		}
		return 0
	}
	categories := map[string]int{}
	var mismatched, steps, emuAborts, modelAborts int
	var firstBad []Step
	for i := range *seeds {
		seed := *firstSeed + uint64(i)
		g := &generator{r: rand.New(rand.NewPCG(seed, 0x5eed)), cfg: cfg}
		sched := g.schedule()
		steps += len(sched)
		emuOut, mdlOut, err := h.run(sched)
		if err != nil {
			log.Printf("seed %d: %v", seed, err)
			return 2
		}
		emuAborts += countAborted(emuOut)
		modelAborts += countAborted(mdlOut)
		d := firstDivergence(sched, emuOut, mdlOut)
		if d == nil {
			continue
		}
		mismatched++
		categories[d.category()]++
		if firstBad == nil {
			log.Printf("seed %d: first mismatch at step %d: %s", seed, d.step, d.detail())
			firstBad = sched
		}
	}

	fmt.Printf("model=%s pushdown=%v schedules=%d steps=%d mismatched=%d aborted(emulator)=%d aborted(model)=%d\n",
		*modelName, *pushdown, *seeds, steps, mismatched, emuAborts, modelAborts)
	if mismatched == 0 {
		return 0
	}
	fmt.Println("first divergence per mismatching schedule, by kind:")
	keys := make([]string, 0, len(categories))
	for k := range categories {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		if categories[keys[i]] != categories[keys[j]] {
			return categories[keys[i]] > categories[keys[j]]
		}
		return keys[i] < keys[j]
	})
	for _, k := range keys {
		fmt.Printf("  %6d  %s\n", categories[k], k)
	}

	small := firstBad
	if !*noShrink {
		if small, err = h.shrink(firstBad); err != nil {
			log.Print(err)
			return 2
		}
	}
	d, err := h.diff(small)
	if err != nil {
		log.Print(err)
		return 2
	}
	fmt.Printf("\nminimal mismatching schedule (%d steps):\n%s", len(small), d.render(small))
	if *dumpFailed != "" {
		var b strings.Builder
		for _, st := range small {
			line, _ := json.Marshal(st)
			b.Write(line)
			b.WriteByte('\n')
		}
		if err := os.WriteFile(*dumpFailed, []byte(b.String()), 0o644); err != nil {
			log.Print(err)
			return 2
		}
	}
	if *mustMatch {
		return 1
	}
	return 0
}

type harness struct {
	ctx context.Context
	emu *emulator
	mdl *model
}

type divergence struct {
	step       int
	sched      []Step
	emu, model []Outcome
}

func (d *divergence) category() string {
	st := d.sched[d.step]
	return fmt.Sprintf("%-10s emulator=%-16s model=%s", st.Op, d.emu[d.step].Kind(), d.model[d.step].Kind())
}

func (d *divergence) detail() string {
	return fmt.Sprintf("%s: emulator=%s model=%s", d.sched[d.step], d.emu[d.step], d.model[d.step])
}

func (d *divergence) render(sched []Step) string {
	var b strings.Builder
	for i, st := range sched {
		mark := "  "
		if d.emu[i] != d.model[i] {
			mark = "!!"
		}
		fmt.Fprintf(&b, "%s %2d  %-40s emulator=%-24s model=%s\n", mark, i, st, d.emu[i], d.model[i])
	}
	return b.String()
}

// run executes a schedule on the emulator and the model.
func (h *harness) run(sched []Step) (emu, mdl []Outcome, err error) {
	if emu, err = h.emu.runSchedule(h.ctx, sched); err != nil {
		return nil, nil, err
	}
	if mdl, err = h.mdl.run(sched); err != nil {
		return nil, nil, err
	}
	return emu, mdl, nil
}

// diff runs a schedule on both sides; nil means every step matched.
func (h *harness) diff(sched []Step) (*divergence, error) {
	emu, mdl, err := h.run(sched)
	if err != nil {
		return nil, err
	}
	return firstDivergence(sched, emu, mdl), nil
}

func firstDivergence(sched []Step, emu, mdl []Outcome) *divergence {
	for i := range sched {
		if emu[i] != mdl[i] {
			return &divergence{step: i, sched: sched, emu: emu, model: mdl}
		}
	}
	return nil
}

func countAborted(out []Outcome) int {
	n := 0
	for _, o := range out {
		if o == errorOutcome("ABORTED") {
			n++
		}
	}
	return n
}

// shrink greedily drops whole transactions, data operations, and mutations
// while the schedule still mismatches.
func (h *harness) shrink(sched []Step) ([]Step, error) {
	for {
		improved := false
		for _, cand := range candidates(sched) {
			d, err := h.diff(cand)
			if err != nil {
				return nil, err
			}
			if d != nil {
				sched, improved = cand, true
				break
			}
		}
		if !improved {
			return sched, nil
		}
	}
}

func candidates(sched []Step) [][]Step {
	var out [][]Step
	seen := map[int]bool{}
	for _, st := range sched {
		if st.Txn != 0 && !seen[st.Txn] {
			seen[st.Txn] = true
			t := st.Txn
			out = append(out, slices.DeleteFunc(slices.Clone(sched), func(s Step) bool { return s.Txn == t }))
		}
	}
	for i, st := range sched {
		switch st.Op {
		case "read", "sql", "dml_insert", "dml_update", "dml_delete":
			out = append(out, slices.Delete(slices.Clone(sched), i, i+1))
		case "commit":
			for j := range st.Muts {
				c := slices.Clone(sched)
				c[i].Muts = slices.Delete(slices.Clone(st.Muts), j, j+1)
				out = append(out, c)
			}
		}
	}
	return out
}

func readSchedule(path string) ([]Step, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var sched []Step
	for _, line := range strings.Split(string(data), "\n") {
		if strings.TrimSpace(line) == "" {
			continue
		}
		var st Step
		if err := json.Unmarshal([]byte(line), &st); err != nil {
			return nil, fmt.Errorf("%s: %w", line, err)
		}
		sched = append(sched, st)
	}
	return sched, nil
}
