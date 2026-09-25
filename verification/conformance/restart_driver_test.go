package main

import (
	"context"
	"fmt"
	"net"
	"slices"
	"sort"
	"strconv"
	"sync"
	"testing"
	"time"

	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	spannerpb "cloud.google.com/go/spanner/apiv1/spannerpb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/emptypb"
	"google.golang.org/protobuf/types/known/structpb"
	"google.golang.org/protobuf/types/known/timestamppb"
)

// A real gRPC endpoint with an injectable durable store. Lifecycle commands
// replace Docker only; runCrashSchedule, Commit probing, reconnect/READY polling,
// timestamp audit reads, candidate fan-out and the Lean oracle are production code.
type restartFake struct {
	spannerpb.UnimplementedSpannerServer
	mu                                                 sync.Mutex
	mode                                               string
	rows                                               map[string]map[int]int64
	audit                                              map[string]time.Time
	commits, sessions, transactions, polls, auditReads int
	entered, killed                                    chan struct{}
}
type restartAdmin struct {
	databasepb.UnimplementedDatabaseAdminServer
	f *restartFake
}

func (a *restartAdmin) GetDatabase(context.Context, *databasepb.GetDatabaseRequest) (*databasepb.Database, error) {
	a.f.mu.Lock()
	defer a.f.mu.Unlock()
	a.f.polls++
	if a.f.polls == 1 {
		return nil, status.Error(codes.Unavailable, "recovering")
	}
	state := databasepb.Database_READY
	if a.f.polls == 2 {
		state = databasepb.Database_CREATING
	}
	return &databasepb.Database{Name: "fake/db", State: state}, nil
}
func (f *restartFake) CreateSession(context.Context, *spannerpb.CreateSessionRequest) (*spannerpb.Session, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.sessions++
	return &spannerpb.Session{Name: fmt.Sprintf("session/%d", f.sessions)}, nil
}
func (f *restartFake) DeleteSession(context.Context, *spannerpb.DeleteSessionRequest) (*emptypb.Empty, error) {
	return &emptypb.Empty{}, nil
}
func (f *restartFake) BeginTransaction(context.Context, *spannerpb.BeginTransactionRequest) (*spannerpb.Transaction, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.transactions++
	return &spannerpb.Transaction{Id: []byte(fmt.Sprint(f.transactions))}, nil
}
func (f *restartFake) Commit(_ context.Context, r *spannerpb.CommitRequest) (*spannerpb.CommitResponse, error) {
	f.mu.Lock()
	f.commits++
	n := f.commits
	ts := time.Unix(int64(100+n), 0)
	if n == 2 && f.mode == "lost_ack_regressed_clock" {
		ts = time.Unix(200, 0)
	}
	apply := n != 2 || (f.mode != "lost" && f.mode != "success_lost" && f.mode != "terminal" && f.mode != "wrong_terminal")
	if apply {
		for _, m := range r.Mutations {
			if d := m.GetDelete(); d != nil {
				if d.Table == "RestartAudit" {
					clear(f.audit)
				} else {
					clear(f.rows[d.Table])
				}
				continue
			}
			w := m.GetInsertOrUpdate()
			if w == nil {
				w = m.GetInsert()
			}
			if w == nil {
				w = m.GetUpdate()
			}
			if w == nil {
				continue
			}
			if n == 2 && f.mode == "partial" && w.Table == "B" {
				continue
			}
			for _, row := range w.Values {
				if w.Table == "RestartAudit" {
					f.audit[row.Values[0].GetStringValue()] = ts
					continue
				}
				k, _ := strconv.Atoi(row.Values[0].GetStringValue())
				v, _ := strconv.ParseInt(row.Values[1].GetStringValue(), 10, 64)
				f.rows[w.Table][k] = v
			}
		}
	}
	if n == 2 {
		close(f.entered)
	}
	mode := f.mode
	f.mu.Unlock()
	if n == 2 {
		switch mode {
		case "terminal":
			return nil, status.Error(codes.AlreadyExists, "duplicate")
		case "wrong_terminal":
			return nil, status.Error(codes.Aborted, "incorrect abort")
		case "success", "success_lost":
		default:
			<-f.killed
			return nil, status.Error(codes.Unavailable, "lost response")
		}
	}
	return &spannerpb.CommitResponse{CommitTimestamp: timestamppb.New(ts)}, nil
}
func (f *restartFake) Read(_ context.Context, r *spannerpb.ReadRequest) (*spannerpb.ResultSet, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	rs := &spannerpb.ResultSet{}
	if r.Table == "RestartAudit" {
		f.auditReads++
		for k, ts := range f.audit {
			rs.Rows = append(rs.Rows, &structpb.ListValue{Values: []*structpb.Value{structpb.NewStringValue(k), structpb.NewStringValue(ts.Format(time.RFC3339Nano))}})
		}
		return rs, nil
	}
	var keys []int
	for k := range f.rows[r.Table] {
		keys = append(keys, k)
	}
	sort.Ints(keys)
	for _, k := range keys {
		rs.Rows = append(rs.Rows, &structpb.ListValue{Values: []*structpb.Value{intValue(int64(k)), intValue(f.rows[r.Table][k])}})
	}
	return rs, nil
}

func TestPersistentCrashDriver(t *testing.T) {
	for _, tc := range []struct {
		name               string
		match, driverError bool
		variants           int
	}{
		{"durable", true, false, 2}, {"lost", true, false, 2},
		{"partial", false, false, 2}, {"success", true, false, 1},
		{"success_lost", false, false, 1}, {"lost_ack_regressed_clock", false, true, 0},
		{"terminal", true, false, 1}, {"wrong_terminal", false, false, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &restartFake{mode: tc.name, rows: map[string]map[int]int64{"A": {}, "B": {}}, audit: map[string]time.Time{}, entered: make(chan struct{}), killed: make(chan struct{})}
			listener, err := net.Listen("tcp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			server := grpc.NewServer()
			spannerpb.RegisterSpannerServer(server, f)
			databasepb.RegisterDatabaseAdminServer(server, &restartAdmin{f: f})
			go func() { _ = server.Serve(listener) }()
			t.Cleanup(server.Stop)
			d := &restartDriver{addr: listener.Addr().String(), mode: "persistent", probe: &commitProbe{sent: make(chan struct{}, 1)}}
			d.command = func(args ...string) (string, error) {
				switch args[0] {
				case "kill":
					select {
					case <-f.entered:
					case <-time.After(5 * time.Second):
						return "", fmt.Errorf("commit never reached fake")
					}
					close(f.killed)
					return "", nil
				case "inspect":
					return "137 false", nil
				case "start":
					return "", nil
				default:
					return "", fmt.Errorf("unexpected command %v", args)
				}
			}
			e, err := dialEmulator(d.addr, grpc.WithStatsHandler(d.probe))
			if err != nil {
				t.Fatal(err)
			}
			e.database = "fake/db"
			t.Cleanup(func() { _ = e.conn.Close() })
			schedule := restartFixture(false, false)
			schedule[3].Durable, schedule[3].Terminal = true, true // driver must derive observations, ignoring input hints
			schedule[3].DelayUS = 20_000
			if tc.name == "terminal" || tc.name == "wrong_terminal" {
				schedule[3].Muts = []Mut{{Kind: "insert", Table: "A", Key: 0, Val: 70}}
			}
			actual, variants, err := e.runCrashSchedule(context.Background(), d, schedule)
			if tc.driverError {
				if err == nil {
					t.Fatal("lost-ack timestamp regression escaped audit")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if len(variants) != tc.variants {
				t.Fatalf("variants=%d want %d", len(variants), tc.variants)
			}
			if d.kills != 1 || f.polls != 3 || f.auditReads != 1 {
				t.Fatalf("kill=%d READY polls=%d audit reads=%d", d.kills, f.polls, f.auditReads)
			}
			if tc.name == "success" || tc.name == "success_lost" {
				if !variants[0][3].Durable || d.completed != 1 {
					t.Fatal("successful reply did not force durability")
				}
			}
			m := testModel(t, "upstream", "persistent")
			match := false
			for _, v := range variants {
				expected, err := m.run(v)
				if err != nil {
					t.Fatal(err)
				}
				match = match || slices.Equal(actual, expected)
			}
			if match != tc.match {
				t.Fatalf("match=%t want %t; actual=%v", match, tc.match, actual)
			}
		})
	}
}
