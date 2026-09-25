// Command stress runs concurrent bank transfers against a Cloud Spanner
// emulator through the Go client, with the client's abort-retry loop, and
// checks the result: the total balance is invariant and every account ends at
// exactly its starting balance plus the transfers that committed (no lost
// updates). It reports throughput and the number of aborted attempts.
//
// Workloads:
//   - contended: every worker transfers between random accounts of a small
//     shared set.
//   - disjoint: each worker owns two accounts; with parallel read-write
//     transactions nothing should abort.
//   - mux282: explicit BeginTransaction plus buffered mutations on multiplexed
//     sessions, concurrently (upstream issue #282: silently lost writes).
//
// See verification/README.md.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"math/rand/v2"
	"os"
	"os/exec"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"cloud.google.com/go/spanner"
	database "cloud.google.com/go/spanner/admin/database/apiv1"
	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	instance "cloud.google.com/go/spanner/admin/instance/apiv1"
	instancepb "cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
)

func main() {
	var (
		image     = flag.String("image", "cloud-spanner-emulator:parallel-rw-txns", "emulator image started when SPANNER_EMULATOR_HOST is unset")
		abortProb = flag.Int("abort-probability", 0, "--abort_current_transaction_probability for the started emulator")
		workers   = flag.Int("workers", 16, "concurrent goroutines")
		accounts  = flag.Int("accounts", 4, "accounts in the contended workload")
		duration  = flag.Duration("duration", 15*time.Second, "duration of each workload")
		workloads = flag.String("workloads", "contended,disjoint,mux282", "comma-separated workloads to run")
		useDML    = flag.Bool("dml", true, "write with DML (UPDATE ... WHERE id = @id) instead of mutations")
	)
	flag.Parse()
	log.SetFlags(log.Ltime)
	ctx := context.Background()

	addr := os.Getenv("SPANNER_EMULATOR_HOST")
	if addr == "" {
		a, stop, err := startDocker(*image, *abortProb)
		if err != nil {
			log.Fatal(err)
		}
		defer stop()
		addr = a
		log.Printf("started %s at %s", *image, addr)
	}
	opts := []option.ClientOption{
		option.WithEndpoint(addr),
		option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials())),
		option.WithoutAuthentication(),
	}
	failed := false
	for _, w := range strings.Split(*workloads, ",") {
		db, err := createDatabase(ctx, opts)
		if err != nil {
			log.Fatal(err)
		}
		client, err := spanner.NewClientWithConfig(ctx, db, spanner.ClientConfig{DisableNativeMetrics: true}, opts...)
		if err != nil {
			log.Fatal(err)
		}
		var res result
		switch w {
		case "contended":
			res, err = transfers(ctx, client, *workers, *duration, *useDML, func(worker int, r *rand.Rand) (int64, int64) {
				from := r.IntN(*accounts)
				to := (from + 1 + r.IntN(*accounts-1)) % *accounts
				return int64(from), int64(to)
			}, *accounts)
		case "disjoint":
			res, err = transfers(ctx, client, *workers, *duration, *useDML, func(worker int, r *rand.Rand) (int64, int64) {
				a, b := int64(2*worker), int64(2*worker+1)
				if r.IntN(2) == 0 {
					return a, b
				}
				return b, a
			}, 2**workers)
		case "mux282":
			res, err = mux282(ctx, client, *workers, *duration)
		default:
			err = fmt.Errorf("unknown workload %q", w)
		}
		client.Close()
		if err != nil {
			log.Printf("%s: FAILED: %v", w, err)
			failed = true
			continue
		}
		fmt.Printf("%-9s workers=%d committed=%d aborted_attempts=%d unfinished=%d txn/s=%.1f abort_rate=%.3f\n",
			w, *workers, res.committed, res.aborts, res.unfinished, float64(res.committed)/res.elapsed.Seconds(),
			float64(res.aborts)/float64(max(1, res.committed+res.aborts)))
	}
	if failed {
		os.Exit(1)
	}
}

type result struct {
	committed, aborts int64
	// Transactions still retrying at the hard deadline: their outcome is
	// unknown, so the checks allow for either.
	unfinished int64
	elapsed    time.Duration
}

// grace is how long a transaction may keep retrying after the workload's
// duration before it is cancelled. Upstream's single lock slot can livelock.
const grace = 30 * time.Second

const initialBalance = 1_000_000

// transfers moves 1 unit between two accounts per transaction until the
// deadline, then verifies every balance against the committed transfers.
func transfers(ctx context.Context, client *spanner.Client, workers int, d time.Duration, useDML bool,
	pick func(worker int, r *rand.Rand) (int64, int64), numAccounts int) (result, error) {
	var muts []*spanner.Mutation
	for a := range numAccounts {
		muts = append(muts, spanner.Insert("Accounts", []string{"Id", "Balance"}, []any{int64(a), int64(initialBalance)}))
	}
	if _, err := client.Apply(ctx, muts); err != nil {
		return result{}, fmt.Errorf("seed: %w", err)
	}

	var committed, attempts, unfinished atomic.Int64
	deltas := make([][]int64, workers)  // per worker, per account
	unknown := make([][]int64, workers) // per worker, per account: unfinished transfers touching it
	errs := make(chan error, workers)
	deadline := time.Now().Add(d)
	wctx, cancel := context.WithDeadline(ctx, deadline.Add(grace))
	defer cancel()
	start := time.Now()
	var wg sync.WaitGroup
	for w := range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			r := rand.New(rand.NewPCG(uint64(w), 7))
			deltas[w] = make([]int64, numAccounts)
			unknown[w] = make([]int64, numAccounts)
			for time.Now().Before(deadline) {
				from, to := pick(w, r)
				_, err := client.ReadWriteTransaction(wctx, func(ctx context.Context, txn *spanner.ReadWriteTransaction) error {
					attempts.Add(1)
					bal := map[int64]int64{}
					for _, id := range []int64{from, to} {
						row, err := txn.ReadRow(ctx, "Accounts", spanner.Key{id}, []string{"Balance"})
						if err != nil {
							return err
						}
						var b int64
						if err := row.Column(0, &b); err != nil {
							return err
						}
						bal[id] = b
					}
					if !useDML {
						return txn.BufferWrite([]*spanner.Mutation{
							spanner.Update("Accounts", []string{"Id", "Balance"}, []any{from, bal[from] - 1}),
							spanner.Update("Accounts", []string{"Id", "Balance"}, []any{to, bal[to] + 1}),
						})
					}
					for _, id := range []int64{from, to} {
						nb := bal[id] + 1
						if id == from {
							nb = bal[id] - 1
						}
						n, err := txn.Update(ctx, spanner.Statement{
							SQL:    "UPDATE Accounts SET Balance = @b WHERE Id = @id",
							Params: map[string]any{"b": nb, "id": id},
						})
						if err != nil {
							return err
						}
						if n != 1 {
							return fmt.Errorf("updated %d rows", n)
						}
					}
					return nil
				})
				if err != nil {
					if wctx.Err() != nil {
						unfinished.Add(1)
						unknown[w][from]++
						unknown[w][to]++
					} else {
						errs <- err
					}
					return
				}
				committed.Add(1)
				deltas[w][from]--
				deltas[w][to]++
			}
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)
	close(errs)
	if err := <-errs; err != nil {
		return result{}, err
	}

	// Verify: each account = initial + committed deltas (up to the unfinished
	// transfers touching it); total unchanged.
	want := make([]int64, numAccounts)
	slack := make([]int64, numAccounts)
	for a := range want {
		want[a] = initialBalance
		for w := range workers {
			want[a] += deltas[w][a]
			slack[a] += unknown[w][a]
		}
	}
	iter := client.Single().Read(ctx, "Accounts", spanner.AllKeys(), []string{"Id", "Balance"})
	var total int64
	seen := 0
	if err := iter.Do(func(row *spanner.Row) error {
		var id, b int64
		if err := row.Columns(&id, &b); err != nil {
			return err
		}
		if b < want[id]-slack[id] || b > want[id]+slack[id] {
			return fmt.Errorf("account %d: balance %d, want %d±%d (lost update)", id, b, want[id], slack[id])
		}
		total += b
		seen++
		return nil
	}); err != nil {
		return result{}, err
	}
	if seen != numAccounts || total != int64(numAccounts)*initialBalance {
		return result{}, fmt.Errorf("total %d over %d accounts, want %d", total, seen, int64(numAccounts)*initialBalance)
	}
	c := committed.Load()
	return result{committed: c, aborts: attempts.Load() - c, unfinished: unfinished.Load(), elapsed: elapsed}, nil
}

// mux282 inserts rows with explicit BeginTransaction and buffered mutations
// (the pattern of upstream issue #282) from concurrent workers, then checks
// that every committed row exists.
func mux282(ctx context.Context, client *spanner.Client, workers int, d time.Duration) (result, error) {
	var committed, aborts, unfinished atomic.Int64
	var mu sync.Mutex
	var ids []int64
	errs := make(chan error, workers)
	deadline := time.Now().Add(d)
	wctx, cancel := context.WithDeadline(ctx, deadline.Add(grace))
	defer cancel()
	start := time.Now()
	var wg sync.WaitGroup
	for w := range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := int64(0); time.Now().Before(deadline); i++ {
				id := int64(w)<<32 | i
				for {
					if wctx.Err() != nil {
						unfinished.Add(1)
						return
					}
					txn, err := spanner.NewReadWriteStmtBasedTransactionWithOptions(wctx, client,
						spanner.TransactionOptions{BeginTransactionOption: spanner.ExplicitBeginTransaction})
					if err != nil {
						if wctx.Err() != nil {
							unfinished.Add(1)
						} else {
							errs <- err
						}
						return
					}
					if err := txn.BufferWrite([]*spanner.Mutation{
						spanner.Insert("Accounts", []string{"Id", "Balance"}, []any{id, int64(1)}),
					}); err != nil {
						errs <- err
						return
					}
					_, err = txn.CommitWithReturnResp(wctx)
					if wctx.Err() != nil {
						unfinished.Add(1)
						return
					}
					if spanner.ErrCode(err) == codes.Aborted {
						aborts.Add(1)
						continue
					}
					if err != nil {
						errs <- err
						return
					}
					committed.Add(1)
					mu.Lock()
					ids = append(ids, id)
					mu.Unlock()
					break
				}
			}
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)
	close(errs)
	if err := <-errs; err != nil {
		return result{}, err
	}
	var missing []int64
	for _, id := range ids {
		_, err := client.Single().ReadRow(ctx, "Accounts", spanner.Key{id}, []string{"Id"})
		if spanner.ErrCode(err) == codes.NotFound {
			missing = append(missing, id)
		} else if err != nil {
			return result{}, err
		}
	}
	if len(missing) > 0 {
		return result{}, fmt.Errorf("%d of %d committed rows lost (e.g. id %d)", len(missing), len(ids), missing[0])
	}
	return result{committed: committed.Load(), aborts: aborts.Load(), unfinished: unfinished.Load(), elapsed: elapsed}, nil
}

func createDatabase(ctx context.Context, opts []option.ClientOption) (string, error) {
	const project = "projects/stress"
	ia, err := instance.NewInstanceAdminClient(ctx, opts...)
	if err != nil {
		return "", err
	}
	defer ia.Close()
	for attempt := 0; ; attempt++ {
		op, err := ia.CreateInstance(ctx, &instancepb.CreateInstanceRequest{
			Parent: project, InstanceId: "stress",
			Instance: &instancepb.Instance{Config: project + "/instanceConfigs/emulator-config", DisplayName: "stress", NodeCount: 1},
		})
		if err == nil {
			_, err = op.Wait(ctx)
		}
		if err == nil || status.Code(err) == codes.AlreadyExists {
			break
		}
		if status.Code(err) != codes.Unavailable || attempt > 50 {
			return "", fmt.Errorf("create instance: %w", err)
		}
		time.Sleep(200 * time.Millisecond)
	}
	da, err := database.NewDatabaseAdminClient(ctx, opts...)
	if err != nil {
		return "", err
	}
	defer da.Close()
	id := fmt.Sprintf("db%d", time.Now().UnixNano()%1_000_000_000)
	op, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
		Parent:          project + "/instances/stress",
		CreateStatement: "CREATE DATABASE `" + id + "`",
		ExtraStatements: []string{"CREATE TABLE Accounts (Id INT64 NOT NULL, Balance INT64 NOT NULL) PRIMARY KEY (Id)"},
	})
	if err != nil {
		return "", err
	}
	if _, err := op.Wait(ctx); err != nil {
		return "", err
	}
	return project + "/instances/stress/databases/" + id, nil
}

// startDocker runs emulator_main directly on an ephemeral host port.
func startDocker(image string, abortProb int) (string, func(), error) {
	out, err := exec.Command("docker", "run", "-d", "--rm", "-p", "127.0.0.1::9010",
		"--entrypoint", "./emulator_main", image, "--host_port", "0.0.0.0:9010",
		fmt.Sprintf("--abort_current_transaction_probability=%d", abortProb)).Output()
	if err != nil {
		return "", nil, fmt.Errorf("docker run: %w", err)
	}
	id := strings.TrimSpace(string(out))
	stop := func() { _ = exec.Command("docker", "rm", "-f", id).Run() }
	port, err := exec.Command("docker", "port", id, "9010/tcp").Output()
	if err != nil {
		stop()
		return "", nil, fmt.Errorf("docker port: %w", err)
	}
	time.Sleep(time.Second)
	return strings.TrimSpace(strings.Split(string(port), "\n")[0]), stop, nil
}
