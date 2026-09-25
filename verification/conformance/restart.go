package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math/rand/v2"
	"net"
	"net/http"
	"os"
	"os/exec"
	"slices"
	"strconv"
	"strings"
	"time"

	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	spannerpb "cloud.google.com/go/spanner/apiv1/spannerpb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/stats"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/timestamppb"
)

type commitProbe struct{ sent chan struct{} }
type methodKey struct{}

func (p *commitProbe) TagRPC(ctx context.Context, info *stats.RPCTagInfo) context.Context {
	return context.WithValue(ctx, methodKey{}, info.FullMethodName)
}
func (p *commitProbe) HandleRPC(ctx context.Context, event stats.RPCStats) {
	if _, ok := event.(*stats.OutPayload); ok && ctx.Value(methodKey{}) == "/google.spanner.v1.Spanner/Commit" {
		select {
		case p.sent <- struct{}{}:
		default:
		}
	}
}
func (p *commitProbe) TagConn(ctx context.Context, _ *stats.ConnTagInfo) context.Context { return ctx }
func (p *commitProbe) HandleConn(context.Context, stats.ConnStats)                       {}

// restartDriver owns exactly one container and one data directory. It never
// discovers or kills containers by image/name patterns. A restart starts the
// same container, mount, command, and host port, after a real SIGKILL.
type restartDriver struct {
	command                                                           func(...string) (string, error) // test lifecycle; RPC path is unchanged
	engine                                                            *http.Client
	probe                                                             *commitProbe
	id, addr, dir, mode, checkpointCommand                            string
	kills, commitAttempts, interrupted, completed, checkpointAttempts int
}

func (d *restartDriver) docker(args ...string) (string, error) {
	if d.command != nil {
		return d.command(args...)
	}
	return dockerCommand(args...)
}

func dockerCommand(args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, "docker", args...).CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("docker %v: %w: %s", args, err, out)
	}
	return strings.TrimSpace(string(out)), nil
}

func newRestartDriver(image, mode, checkpoint string) (*restartDriver, error) {
	// Reserve an unused loopback port. Exclude the shared development ports even
	// on hosts whose ephemeral range has been customized.
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	port := listener.Addr().(*net.TCPAddr).Port
	_ = listener.Close()
	if port == 9010 || port == 9020 {
		return nil, fmt.Errorf("refusing shared port %d", port)
	}
	dir, err := os.MkdirTemp("", "spanner-l2-")
	if err != nil {
		return nil, err
	}
	d := &restartDriver{probe: &commitProbe{sent: make(chan struct{}, 1)}, addr: net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), dir: dir, mode: mode, checkpointCommand: checkpoint}
	// Direct local Engine API avoids CLI startup latency on the kill path.
	host := os.Getenv("DOCKER_HOST")
	if host == "" {
		host, err = d.docker("context", "inspect", "--format", "{{.Endpoints.docker.Host}}")
		if err != nil {
			_ = os.RemoveAll(dir)
			return nil, err
		}
	}
	if strings.HasPrefix(host, "unix://") {
		socket := strings.TrimPrefix(host, "unix://")
		d.engine = &http.Client{Timeout: 30 * time.Second, Transport: &http.Transport{DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			return (&net.Dialer{}).DialContext(ctx, "unix", socket)
		}}}
	}
	args := []string{"create", "--cpus=2", "-p", d.addr + ":9010", "--mount", "type=bind,src=" + dir + ",dst=/data", "--entrypoint", "./emulator_main", image,
		"--host_port", "0.0.0.0:9010", "--abort_current_transaction_probability=0"}
	if mode == "persistent" {
		args = append(args, "--data_dir=/data")
	}
	d.id, err = d.docker(args...)
	if err != nil {
		_ = os.RemoveAll(dir)
		return nil, err
	}
	if _, err = d.docker("start", d.id); err != nil {
		d.close()
		return nil, err
	}
	return d, nil
}

func (d *restartDriver) close() {
	if d.engine != nil {
		d.engine.CloseIdleConnections()
	}
	if d.id != "" {
		_, _ = d.docker("rm", "-f", d.id)
	}
	_ = os.RemoveAll(d.dir)
}

func (d *restartDriver) kill() error {
	if d.engine != nil {
		resp, err := d.engine.Post("http://docker/containers/"+d.id+"/kill?signal=KILL", "", nil)
		if err != nil {
			return err
		}
		body, _ := io.ReadAll(resp.Body)
		_ = resp.Body.Close()
		if resp.StatusCode != http.StatusNoContent {
			return fmt.Errorf("docker kill HTTP %d: %s", resp.StatusCode, body)
		}
	} else if _, err := d.docker("kill", "--signal=KILL", d.id); err != nil {
		return err
	}
	state, err := d.docker("inspect", "--format", "{{.State.ExitCode}} {{.State.Running}}", d.id)
	if err != nil {
		return err
	}
	if state != "137 false" {
		return fmt.Errorf("SIGKILL not confirmed: %s", state)
	}
	d.kills++
	return nil
}

func (d *restartDriver) restart(ctx context.Context, e *emulator) error {
	oldDB := e.database
	_ = e.conn.Close()
	if _, err := d.docker("start", d.id); err != nil {
		return err
	}
	fresh, err := dialEmulator(d.addr, grpc.WithStatsHandler(d.probe))
	if err != nil {
		return err
	}
	*e = *fresh
	e.database = oldDB
	da := databasepb.NewDatabaseAdminClient(e.conn)
	var lastErr error
	for deadline := time.Now().Add(60 * time.Second); time.Now().Before(deadline); {
		cctx, cancel := context.WithTimeout(ctx, time.Second)
		db, err := da.GetDatabase(cctx, &databasepb.GetDatabaseRequest{Name: oldDB})
		cancel()
		lastErr = err
		if status.Code(err) == codes.Unavailable || status.Code(err) == codes.DeadlineExceeded {
			e.conn.ResetConnectBackoff()
			time.Sleep(50 * time.Millisecond)
			continue
		}
		if d.mode == "no-persistence" {
			if status.Code(err) != codes.NotFound {
				return fmt.Errorf("restart must lose old database %s: got %v, %v", oldDB, db, err)
			}
			return e.setup(ctx, restartAuditDDL)
		}
		if err != nil {
			return fmt.Errorf("persistent recovery of %s: %w", oldDB, err)
		}
		if db.State == databasepb.Database_READY {
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	state, _ := d.docker("inspect", "--format", "{{.State.Status}} exit={{.State.ExitCode}}", d.id)
	logs, _ := d.docker("logs", "--tail", "12", d.id)
	return fmt.Errorf("restart did not become ready: %s; last RPC: %v; logs: %s", state, lastErr, logs)
}

// crashSchedule cuts a random interleaving, loses its open transactions, then
// reads BOTH recovered tables before any post-restart writes. Every schedule
// includes later transactions and final scans, so neither persistence nor
// torn commits can hide behind a setup transaction clearing the database.
func (g *generator) crashSchedule() []Step {
	before := g.schedule()
	cut := 2 + g.r.IntN(len(before)-2)
	out := slices.Clone(before[:cut])
	crash := Step{Op: "crash", DelayUS: g.r.IntN(3001)}
	if g.r.IntN(2) == 0 {
		// Use an existing transaction's next Commit where available.
		for i := cut; i < len(before); i++ {
			if before[i].Op == "commit" {
				out = slices.Clone(before[:i])
				crash.Txn, crash.Muts, crash.DuringCommit = before[i].Txn, before[i].Muts, true
				break
			}
		}
	}
	if g.r.IntN(4) == 0 {
		// A large, conflict-free RPC makes actual in-flight interruptions observable
		// without emulator fault-injection code. Duplicate resolved rows are legal.
		out = slices.Clone(before[:2])
		out = append(out, Step{Txn: 90, Op: "begin_rw"})
		crash.Txn, crash.DuringCommit = 90, true
		crash.Muts = nil
		for i := 0; i < 8000; i++ {
			crash.Muts = append(crash.Muts, Mut{Kind: "upsert", Table: "A", Key: i % g.cfg.numKeys, Val: int64(i)})
		}
	}
	at := 1
	out = append(out, Step{Txn: 98, Op: "begin_ro", At: &at}, Step{Txn: 98, Op: "read", Table: "A", Keys: &Keys{All: true}})
	out = append(out, crash, Step{Op: "restart"}, Step{Txn: 99, Op: "begin_ro", At: &at, Old: true}, Step{Txn: 100, Op: "begin_ro"})
	for _, table := range tables {
		out = append(out, Step{Txn: 100, Op: "read", Table: table, Keys: &Keys{All: true}})
	}
	after := g.schedule()
	for _, st := range after[2:] {
		st.Txn += 100
		st.At = nil
		out = append(out, st)
	}
	return out
}

// runCrashSchedule records which in-flight commits are ambiguous. The oracle
// explores BOTH durable/lost outcomes only for interrupted RPCs. Successful
// commit replies force durability; terminal constraint/abort errors are executed and compared in the model.
func (e *emulator) runCrashSchedule(ctx context.Context, d *restartDriver, steps []Step) ([]Outcome, [][]Step, error) {
	txns := map[int]*txnState{}
	var times []*timestamppb.Timestamp
	var lastObserved *timestamppb.Timestamp
	var oldTimes []*timestamppb.Timestamp
	observe := func(ts *timestamppb.Timestamp) {
		if ts != nil && (lastObserved == nil || lastObserved.AsTime().Before(ts.AsTime())) {
			lastObserved = ts
		}
	}
	e.client = &restartClient{SpannerClient: spannerpb.NewSpannerClient(e.conn), observe: observe, clear: true}
	checkTimestamp := func() error {
		ts := times[len(times)-1]
		if lastObserved != nil && !lastObserved.AsTime().Before(ts.AsTime()) {
			return fmt.Errorf("commit timestamp regressed across restart: %v <= %v", ts, lastObserved)
		}
		lastObserved = ts
		return nil
	}
	candidates := [][]Step{slices.Clone(steps)}
	for i := range candidates[0] {
		candidates[0][i].Durable = false
		candidates[0][i].Terminal = false
	}
	out := make([]Outcome, 0, len(steps))
	running := true
	defer func() {
		if running {
			for _, t := range txns {
				cctx, cancel := context.WithTimeout(context.Background(), callTimeout)
				_, _ = e.client.DeleteSession(cctx, &spannerpb.DeleteSessionRequest{Name: t.session})
				cancel()
			}
		}
	}()
	for i, st := range steps {
		if st.Op == "restart" {
			if running {
				return nil, nil, fmt.Errorf("restart at step %d without crash", i)
			}
			if err := d.restart(ctx, e); err != nil {
				return nil, nil, err
			}
			e.client = &restartClient{SpannerClient: spannerpb.NewSpannerClient(e.conn), observe: observe}
			if err := e.readRestartAudit(ctx, observe); err != nil {
				return nil, nil, err
			}
			running = true
			txns = map[int]*txnState{}
			if d.mode == "no-persistence" {
				times = nil
			}
			out = append(out, okOutcome())
			continue
		}
		if st.Op == "crash" {
			if !running {
				return nil, nil, fmt.Errorf("double crash at step %d", i)
			}
			oldTimes = slices.Clone(times)
			crashResult := okOutcome()
			var response Outcome
			var callErr error
			done := make(chan struct{})
			if st.DuringCommit {
				t := txns[st.Txn]
				if t == nil {
					return nil, nil, fmt.Errorf("crash commit has no transaction %d", st.Txn)
				}
				d.commitAttempts++
				select {
				case <-d.probe.sent:
				default:
				}
				call := st
				call.Op = "commit"
				go func() {
					defer close(done)
					cctx, cancel := context.WithTimeout(ctx, callTimeout)
					defer cancel()
					response, callErr = e.exec(cctx, call, t, &times)
				}()
				// Wait until gRPC has actually sent the Commit payload, or returned
				// a result, before starting the crash timer. No emulator test hooks.
				select {
				case <-d.probe.sent:
				case <-done:
				}
			} else {
				close(done)
			}
			if d.checkpointCommand != "" && !st.DuringCommit {
				// Future Phase 2 checkpoint trigger; must return after requesting a
				// checkpoint. Kill delay spans its asynchronous write/publication stages.
				if _, err := d.docker("exec", d.id, "sh", "-c", d.checkpointCommand); err != nil {
					<-done
					return nil, nil, err
				}
				d.checkpointAttempts++
			}
			time.Sleep(time.Duration(st.DelayUS) * time.Microsecond)
			if err := d.kill(); err != nil {
				<-done
				return nil, nil, err
			}
			running = false
			<-done
			if callErr != nil {
				return nil, nil, callErr
			}
			if st.DuringCommit {
				committed := response.Kind() == "committed"
				ambiguous := response == errorOutcome("UNAVAILABLE") || response == errorOutcome("DEADLINEEXCEEDED") || response == errorOutcome("CANCELED")
				if committed {
					d.completed++
					if err := checkTimestamp(); err != nil {
						return nil, nil, err
					}
				} else if ambiguous {
					d.interrupted++
				} else {
					switch response {
					case errorOutcome("ABORTED"), errorOutcome("ALREADY_EXISTS"), errorOutcome("NOT_FOUND"), errorOutcome("INVALID_ARGUMENT"), errorOutcome("FAILED_PRECONDITION"):
						crashResult = response
						for _, c := range candidates {
							c[i].Terminal = true
						}
					default:
						return nil, nil, fmt.Errorf("unexpected crash commit result %s", response)
					}
				}
				// e.exec counted a successful response as acknowledged. Crash's public
				// result is opaque; remove it from ordinal indices, retain its timestamp
				// in lastObserved for the physical monotonicity check across restarts.
				if committed {
					times = times[:len(times)-1]
				}
				if d.mode == "persistent" {
					if ambiguous {
						if len(candidates) >= 256 {
							return nil, nil, fmt.Errorf("too many ambiguous commits (limit 8 per replay)")
						}
						for _, c := range slices.Clone(candidates) {
							copy := slices.Clone(c)
							copy[i].Durable = true
							candidates = append(candidates, copy)
						}
					} else if committed {
						for _, c := range candidates {
							c[i].Durable = true
						}
					}
				}
			}
			out = append(out, crashResult)
			continue
		}
		if !running {
			out = append(out, errorOutcome("FAILED_PRECONDITION"))
			continue
		}
		t := txns[st.Txn]
		if t == nil {
			cctx, cancel := context.WithTimeout(ctx, callTimeout)
			session, err := e.client.CreateSession(cctx, &spannerpb.CreateSessionRequest{Database: e.database})
			cancel()
			if err != nil {
				return nil, nil, err
			}
			t = &txnState{session: session.Name}
			txns[st.Txn] = t
		}
		cctx, cancel := context.WithTimeout(ctx, callTimeout)
		readTimes := &times
		if st.Old {
			readTimes = &oldTimes
		}
		r, err := e.exec(cctx, st, t, readTimes)
		cancel()
		if err != nil {
			return nil, nil, err
		}
		if r.Kind() == "committed" {
			if err := checkTimestamp(); err != nil {
				return nil, nil, err
			}
		}
		out = append(out, r)
	}
	return out, candidates, nil
}

func runRestarts(mode, image, checkpoint, modelPath, modelName string, pushdown, mustMatch bool, seeds int, first uint64, cfg genConfig, replay, dump string) int {
	if mode != "no-persistence" && mode != "persistent" {
		log.Printf("invalid persistence mode %q", mode)
		return 2
	}
	if os.Getenv("SPANNER_EMULATOR_HOST") != "" {
		log.Print("crash mode requires its own container; unset SPANNER_EMULATOR_HOST")
		return 2
	}
	d, err := newRestartDriver(image, mode, checkpoint)
	if err != nil {
		log.Print(err)
		return 2
	}
	defer d.close()
	log.Printf("crash driver image=%s mode=%s address=%s container=%s", image, mode, d.addr, d.id)
	e, err := dialEmulator(d.addr, grpc.WithStatsHandler(d.probe))
	if err != nil {
		log.Print(err)
		return 2
	}
	defer func() { _ = e.conn.Close() }()
	ctx := context.Background()
	if err = e.setup(ctx, restartAuditDDL); err != nil {
		log.Print(err)
		return 2
	}
	m, err := startModel(modelPath, modelName, pushdown, mode)
	if err != nil {
		log.Print(err)
		return 2
	}
	defer m.close()
	mismatches, total := 0, 0
	if replay != "" {
		seeds = 1
	}
	for i := 0; i < seeds; i++ {
		seed := first + uint64(i)
		g := &generator{r: rand.New(rand.NewPCG(seed, 0x5eed)), cfg: cfg}
		sched := g.crashSchedule()
		if replay != "" {
			sched, err = readSchedule(replay)
			if err != nil {
				log.Print(err)
				return 2
			}
		}
		actual, variants, err := e.runCrashSchedule(ctx, d, sched)
		if err != nil {
			log.Printf("seed %d: %v", seed, err)
			return 2
		}
		total += len(sched)
		matched := false
		var expected []Outcome
		for _, variant := range variants {
			expected, err = m.run(variant)
			if err != nil {
				log.Printf("seed %d model: %v", seed, err)
				return 2
			}
			if slices.Equal(actual, expected) {
				matched = true
				break
			}
		}
		if !matched {
			mismatches++
			if mismatches == 1 {
				diff := &divergence{sched: sched, emu: actual, model: expected}
				log.Printf("seed %d:\n%s", seed, diff.render(sched))
				if dump != "" {
					var b strings.Builder
					for _, st := range sched {
						line, _ := json.Marshal(st)
						b.Write(line)
						b.WriteByte('\n')
					}
					if err = os.WriteFile(dump, []byte(b.String()), 0644); err != nil {
						log.Print(err)
						return 2
					}
				}
			}
		}
		if (i+1)%10 == 0 {
			log.Printf("completed %d/%d seeds; mismatches=%d SIGKILL=%d interrupted-commit=%d", i+1, seeds, mismatches, d.kills, d.interrupted)
		}
	}
	fmt.Printf("persistence=%s model=%s seeds=%d first_seed=%d steps=%d mismatches=%d sigkills=%d commit_attempts=%d interrupted_commits=%d completed_before_kill=%d checkpoint_attempts=%d\n", mode, modelName, seeds, first, total, mismatches, d.kills, d.commitAttempts, d.interrupted, d.completed, d.checkpointAttempts)
	if mismatches > 0 && mustMatch {
		return 1
	}
	return 0
}
