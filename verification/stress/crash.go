package main

import (
	"context"
	"fmt"
	"log"
	"math/rand/v2"
	"net"
	"os"
	"os/exec"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"cloud.google.com/go/spanner"
	"google.golang.org/api/option"
)

// nativeEmulator is an emulator_main process this harness owns.
type nativeEmulator struct {
	binary, addr, logPath string
	args                  []string
	cmd                   *exec.Cmd
	exited                chan error
}

func startNative(binary, dataDir string, port, abortProb int) (*nativeEmulator, error) {
	logs, err := os.CreateTemp("", "stress-emulator-")
	if err != nil {
		return nil, err
	}
	_ = logs.Close()
	e := &nativeEmulator{binary: binary, addr: fmt.Sprintf("localhost:%d", port), logPath: logs.Name()}
	e.args = []string{"--host_port", e.addr, fmt.Sprintf("--abort_current_transaction_probability=%d", abortProb)}
	if dataDir != "" {
		e.args = append(e.args, "--data_dir="+dataDir)
	}
	return e, e.start()
}

func (e *nativeEmulator) start() error {
	logs, err := os.OpenFile(e.logPath, os.O_WRONLY|os.O_APPEND, 0)
	if err != nil {
		return err
	}
	cmd := exec.Command(e.binary, e.args...)
	cmd.Stdout, cmd.Stderr = logs, logs
	if err := cmd.Start(); err != nil {
		_ = logs.Close()
		return err
	}
	e.cmd, e.exited = cmd, make(chan error, 1)
	go func() {
		e.exited <- cmd.Wait()
		_ = logs.Close()
	}()
	for deadline := time.Now().Add(60 * time.Second); time.Now().Before(deadline); time.Sleep(20 * time.Millisecond) {
		select {
		case err := <-e.exited:
			e.exited <- err
			data, _ := os.ReadFile(e.logPath)
			return fmt.Errorf("emulator exited: %v: %s", err, data)
		default:
		}
		if c, err := net.Dial("tcp", e.addr); err == nil {
			_ = c.Close()
			return nil
		}
	}
	return fmt.Errorf("emulator did not listen on %s", e.addr)
}

// kill9 SIGKILLs the process and confirms it died of it.
func (e *nativeEmulator) kill9() error {
	if err := e.cmd.Process.Signal(syscall.SIGKILL); err != nil {
		return err
	}
	<-e.exited
	if ws, ok := e.cmd.ProcessState.Sys().(syscall.WaitStatus); !ok || !ws.Signaled() || ws.Signal() != syscall.SIGKILL {
		return fmt.Errorf("SIGKILL not confirmed: %v", e.cmd.ProcessState)
	}
	return nil
}

func (e *nativeEmulator) stop() {
	if e.cmd != nil && e.cmd.ProcessState == nil {
		_ = e.cmd.Process.Signal(syscall.SIGKILL)
		<-e.exited
	}
	_ = os.Remove(e.logPath)
}

type transfer struct {
	id       string
	from, to int64
}

// crashTransfers runs the transfer workload, kills the emulator with SIGKILL
// in the middle, restarts it on the same data directory, and carries on.
// Every transfer also inserts a Transfers row, so the recovered state can be
// checked exactly: in one snapshot, each balance equals its initial value
// plus the transfers present; every transfer acknowledged before the kill is
// present; and no transfer is present that was never attempted. Commits in
// flight at the kill are ambiguous: either outcome is allowed, and the
// Transfers table says which one happened.
func crashTransfers(ctx context.Context, opts []option.ClientOption, emu *nativeEmulator, workers, accounts int, d, killAfter time.Duration) (result, error) {
	numAccounts := max(accounts, 2*workers)
	db, err := createDatabase(ctx, opts, "CREATE TABLE Transfers (Id STRING(64) NOT NULL, FromId INT64 NOT NULL, ToId INT64 NOT NULL) PRIMARY KEY (Id)")
	if err != nil {
		return result{}, err
	}
	newClient := func() (*spanner.Client, error) {
		return spanner.NewClientWithConfig(ctx, db, spanner.ClientConfig{DisableNativeMetrics: true}, opts...)
	}
	first, err := newClient()
	if err != nil {
		return result{}, err
	}
	var current atomic.Pointer[spanner.Client]
	current.Store(first)
	var muts []*spanner.Mutation
	for a := range numAccounts {
		muts = append(muts, spanner.Insert("Accounts", []string{"Id", "Balance"}, []any{int64(a), int64(initialBalance)}))
	}
	if _, err := first.Apply(ctx, muts); err != nil {
		return result{}, fmt.Errorf("seed: %w", err)
	}

	var mu sync.Mutex
	attempted := map[string]transfer{}
	acked := map[string]bool{}
	var ackedBeforeKill []string
	var committed, attempts, ambiguous atomic.Int64
	// Errors are expected only between the kill and a little after the restart.
	var windowStart, windowEnd atomic.Int64
	windowStart.Store(1 << 62)
	windowEnd.Store(1 << 62)
	inWindow := func() bool {
		now := time.Now().UnixNano()
		return now >= windowStart.Load() && now <= windowEnd.Load()
	}

	deadline := time.Now().Add(d)
	wctx, cancel := context.WithDeadline(ctx, deadline.Add(grace))
	defer cancel()
	errs := make(chan error, workers+1)
	start := time.Now()
	var wg sync.WaitGroup
	for w := range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			r := rand.New(rand.NewPCG(uint64(w), 11))
			for i := 0; time.Now().Before(deadline); i++ {
				t := transfer{id: fmt.Sprintf("%d-%d", w, i), from: int64(r.IntN(numAccounts))}
				t.to = (t.from + 1 + int64(r.IntN(numAccounts-1))) % int64(numAccounts)
				mu.Lock()
				attempted[t.id] = t
				mu.Unlock()
				_, err := current.Load().ReadWriteTransaction(wctx, func(ctx context.Context, txn *spanner.ReadWriteTransaction) error {
					attempts.Add(1)
					var from, to int64
					for id, b := range map[int64]*int64{t.from: &from, t.to: &to} {
						row, err := txn.ReadRow(ctx, "Accounts", spanner.Key{id}, []string{"Balance"})
						if err != nil {
							return err
						}
						if err := row.Column(0, b); err != nil {
							return err
						}
					}
					return txn.BufferWrite([]*spanner.Mutation{
						spanner.Update("Accounts", []string{"Id", "Balance"}, []any{t.from, from - 1}),
						spanner.Update("Accounts", []string{"Id", "Balance"}, []any{t.to, to + 1}),
						spanner.Insert("Transfers", []string{"Id", "FromId", "ToId"}, []any{t.id, t.from, t.to}),
					})
				})
				if err == nil {
					committed.Add(1)
					mu.Lock()
					acked[t.id] = true
					mu.Unlock()
					continue
				}
				ambiguous.Add(1)
				if wctx.Err() != nil {
					return
				}
				if !inWindow() {
					errs <- fmt.Errorf("transfer %s outside the restart window: %w", t.id, err)
					return
				}
				time.Sleep(50 * time.Millisecond)
			}
		}()
	}

	verify := func(client *spanner.Client, mustHave []string) (int64, error) {
		txn := client.ReadOnlyTransaction()
		defer txn.Close()
		present := map[string]transfer{}
		if err := txn.Read(ctx, "Transfers", spanner.AllKeys(), []string{"Id", "FromId", "ToId"}).Do(func(row *spanner.Row) error {
			var t transfer
			if err := row.Columns(&t.id, &t.from, &t.to); err != nil {
				return err
			}
			present[t.id] = t
			return nil
		}); err != nil {
			return 0, err
		}
		want := make([]int64, numAccounts)
		for a := range want {
			want[a] = initialBalance
		}
		mu.Lock()
		var recovered int64
		for id, t := range present {
			if attempted[id] != t {
				mu.Unlock()
				return 0, fmt.Errorf("transfer %s is present but was never attempted as %v", id, t)
			}
			if !acked[id] {
				recovered++
			}
			want[t.from]--
			want[t.to]++
		}
		mu.Unlock()
		for _, id := range mustHave {
			if _, ok := present[id]; !ok {
				return 0, fmt.Errorf("acknowledged transfer %s was lost", id)
			}
		}
		var total int64
		seen := 0
		if err := txn.Read(ctx, "Accounts", spanner.AllKeys(), []string{"Id", "Balance"}).Do(func(row *spanner.Row) error {
			var id, b int64
			if err := row.Columns(&id, &b); err != nil {
				return err
			}
			if b != want[id] {
				return fmt.Errorf("account %d: balance %d, want exactly %d", id, b, want[id])
			}
			total += b
			seen++
			return nil
		}); err != nil {
			return 0, err
		}
		if seen != numAccounts || total != int64(numAccounts)*initialBalance {
			return 0, fmt.Errorf("total %d over %d accounts, want %d", total, seen, int64(numAccounts)*initialBalance)
		}
		return recovered, nil
	}

	var afterRestart int64
	wg.Add(1)
	go func() {
		defer wg.Done()
		time.Sleep(killAfter)
		mu.Lock()
		for id := range acked {
			ackedBeforeKill = append(ackedBeforeKill, id)
		}
		mu.Unlock()
		windowStart.Store(time.Now().UnixNano())
		if err := emu.kill9(); err != nil {
			errs <- err
			return
		}
		killed := time.Now()
		if err := emu.start(); err != nil {
			errs <- err
			return
		}
		client, err := newClient()
		if err != nil {
			errs <- err
			return
		}
		old := current.Swap(client)
		recovered, err := verify(client, ackedBeforeKill)
		if err != nil {
			errs <- fmt.Errorf("right after the restart: %w", err)
			return
		}
		afterRestart = recovered
		windowEnd.Store(time.Now().Add(5 * time.Second).UnixNano())
		log.Printf("killed with %d acknowledged transfers; restarted and verified in %v; %d in-flight transfers had committed",
			len(ackedBeforeKill), time.Since(killed).Round(time.Millisecond), recovered)
		time.AfterFunc(10*time.Second, old.Close)
	}()
	wg.Wait()
	elapsed := time.Since(start)
	close(errs)
	if err := <-errs; err != nil {
		return result{}, err
	}
	client := current.Load()
	defer client.Close()
	mu.Lock()
	var all []string
	for id := range acked {
		all = append(all, id)
	}
	mu.Unlock()
	recovered, err := verify(client, all)
	if err != nil {
		return result{}, fmt.Errorf("at the end: %w", err)
	}
	c := committed.Load()
	return result{committed: c, aborts: attempts.Load() - c - ambiguous.Load(), unfinished: ambiguous.Load(), recovered: max(recovered, afterRestart), elapsed: elapsed}, nil
}
