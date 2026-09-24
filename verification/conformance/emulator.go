package main

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"strconv"
	"strings"
	"time"

	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	instancepb "cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	spannerpb "cloud.google.com/go/spanner/apiv1/spannerpb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/structpb"
	"google.golang.org/protobuf/types/known/timestamppb"
)

const callTimeout = 10 * time.Second

var schemaDDL = []string{
	"CREATE TABLE A (K INT64 NOT NULL, V INT64) PRIMARY KEY (K)",
	"CREATE TABLE B (K INT64 NOT NULL, V INT64) PRIMARY KEY (K)",
}

// startDocker runs emulator_main directly (the gateway does not forward
// --abort_current_transaction_probability) and returns its host:port and a
// stop function.
func startDocker(image string, abortProb int) (string, func(), error) {
	out, err := exec.Command("docker", "run", "-d", "--rm", "-p", "127.0.0.1::9010",
		"--entrypoint", "./emulator_main", image,
		"--host_port", "0.0.0.0:9010",
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
	addr := strings.TrimSpace(strings.Split(string(port), "\n")[0])
	return addr, stop, nil
}

type emulator struct {
	conn     *grpc.ClientConn
	client   spannerpb.SpannerClient
	database string
}

func dialEmulator(addr string, options ...grpc.DialOption) (*emulator, error) {
	options = append(options, grpc.WithTransportCredentials(insecure.NewCredentials()))
	conn, err := grpc.NewClient(addr, options...)
	if err != nil {
		return nil, err
	}
	return &emulator{conn: conn, client: spannerpb.NewSpannerClient(conn)}, nil
}

// setup creates a fresh instance and database with the conformance schema.
func (e *emulator) setup(ctx context.Context) error {
	const project = "projects/conformance"
	inst := project + "/instances/txn"
	ia := instancepb.NewInstanceAdminClient(e.conn)
	var err error
	for attempt := 0; attempt < 50; attempt++ {
		_, err = ia.CreateInstance(ctx, &instancepb.CreateInstanceRequest{
			Parent:     project,
			InstanceId: "txn",
			Instance: &instancepb.Instance{
				Config: project + "/instanceConfigs/emulator-config", DisplayName: "txn", NodeCount: 1,
			},
		})
		if err == nil || status.Code(err) == codes.AlreadyExists {
			err = nil
			break
		}
		if status.Code(err) != codes.Unavailable {
			return fmt.Errorf("create instance: %w", err)
		}
		time.Sleep(200 * time.Millisecond) // emulator still starting
	}
	if err != nil {
		return fmt.Errorf("create instance: %w", err)
	}

	da := databasepb.NewDatabaseAdminClient(e.conn)
	dbID := fmt.Sprintf("db%d", time.Now().UnixNano()%1_000_000_000)
	if _, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
		Parent: inst, CreateStatement: "CREATE DATABASE `" + dbID + "`", ExtraStatements: schemaDDL,
	}); err != nil {
		return fmt.Errorf("create database: %w", err)
	}
	e.database = inst + "/databases/" + dbID
	for range 100 {
		db, err := da.GetDatabase(ctx, &databasepb.GetDatabaseRequest{Name: e.database})
		if err == nil && db.State == databasepb.Database_READY {
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	return errors.New("database never became READY")
}

// codeName maps a gRPC status to the model's code names.
func codeName(err error) string {
	c := status.Code(err)
	switch c {
	case codes.Aborted:
		return "ABORTED"
	case codes.AlreadyExists:
		return "ALREADY_EXISTS"
	case codes.NotFound:
		return "NOT_FOUND"
	case codes.InvalidArgument:
		return "INVALID_ARGUMENT"
	case codes.FailedPrecondition:
		return "FAILED_PRECONDITION"
	default:
		return strings.ToUpper(c.String())
	}
}

func intValue(n int64) *structpb.Value { return structpb.NewStringValue(strconv.FormatInt(n, 10)) }

func keyList(k int) *structpb.ListValue {
	return &structpb.ListValue{Values: []*structpb.Value{intValue(int64(k))}}
}

func keySet(k *Keys) *spannerpb.KeySet {
	switch {
	case k.All:
		return &spannerpb.KeySet{All: true}
	case k.Point != nil:
		return &spannerpb.KeySet{Keys: []*structpb.ListValue{keyList(*k.Point)}}
	}
	r := &spannerpb.KeyRange{}
	if k.LoIncl {
		r.StartKeyType = &spannerpb.KeyRange_StartClosed{StartClosed: keyList(k.Lo)}
	} else {
		r.StartKeyType = &spannerpb.KeyRange_StartOpen{StartOpen: keyList(k.Lo)}
	}
	if k.HiIncl {
		r.EndKeyType = &spannerpb.KeyRange_EndClosed{EndClosed: keyList(k.Hi)}
	} else {
		r.EndKeyType = &spannerpb.KeyRange_EndOpen{EndOpen: keyList(k.Hi)}
	}
	return &spannerpb.KeySet{Ranges: []*spannerpb.KeyRange{r}}
}

// predicate renders a key set as a SQL condition on K plus its parameters.
func predicate(k *Keys, params map[string]*structpb.Value) string {
	switch {
	case k.All:
		return "TRUE"
	case k.Point != nil:
		params["k"] = intValue(int64(*k.Point))
		return "K = @k"
	}
	params["lo"], params["hi"] = intValue(int64(k.Lo)), intValue(int64(k.Hi))
	lo, hi := ">", "<"
	if k.LoIncl {
		lo = ">="
	}
	if k.HiIncl {
		hi = "<="
	}
	return fmt.Sprintf("K %s @lo AND K %s @hi", lo, hi)
}

func paramTypes(params map[string]*structpb.Value) map[string]*spannerpb.Type {
	t := map[string]*spannerpb.Type{}
	for name := range params {
		t[name] = &spannerpb.Type{Code: spannerpb.TypeCode_INT64}
	}
	return t
}

func rowsOf(rs *spannerpb.ResultSet) ([][2]int64, error) {
	var rows [][2]int64
	for _, row := range rs.Rows {
		if len(row.Values) != 2 {
			return nil, fmt.Errorf("row has %d columns", len(row.Values))
		}
		var r [2]int64
		for i, v := range row.Values {
			n, err := strconv.ParseInt(v.GetStringValue(), 10, 64)
			if err != nil {
				return nil, fmt.Errorf("column %d: %q: %w", i, v.GetStringValue(), err)
			}
			r[i] = n
		}
		rows = append(rows, r)
	}
	return rows, nil
}

func mutationProto(m Mut) *spannerpb.Mutation {
	if m.Kind == "delete" {
		return &spannerpb.Mutation{Operation: &spannerpb.Mutation_Delete_{Delete: &spannerpb.Mutation_Delete{
			Table: m.Table, KeySet: &spannerpb.KeySet{Keys: []*structpb.ListValue{keyList(m.Key)}},
		}}}
	}
	w := &spannerpb.Mutation_Write{
		Table: m.Table, Columns: []string{"K", "V"},
		Values: []*structpb.ListValue{{Values: []*structpb.Value{intValue(int64(m.Key)), intValue(m.Val)}}},
	}
	switch m.Kind {
	case "insert":
		return &spannerpb.Mutation{Operation: &spannerpb.Mutation_Insert{Insert: w}}
	case "update":
		return &spannerpb.Mutation{Operation: &spannerpb.Mutation_Update{Update: w}}
	default: // upsert
		return &spannerpb.Mutation{Operation: &spannerpb.Mutation_InsertOrUpdate{InsertOrUpdate: w}}
	}
}

// txnState is the harness's handle on one schedule transaction.
type txnState struct {
	session string
	id      []byte
	rw      bool
	seqno   int64
	done    bool
}

// runSchedule executes steps in order from this goroutine and returns one
// outcome per step. Each transaction gets its own session: a session closes
// its earlier transactions when a later one is used.
func (e *emulator) runSchedule(ctx context.Context, steps []Step) ([]Outcome, error) {
	txns := map[int]*txnState{}
	var commitTs []*timestamppb.Timestamp // commitTs[i] = timestamp of commit i+1
	defer func() {
		for _, t := range txns {
			cctx, cancel := context.WithTimeout(context.Background(), callTimeout)
			if t.rw && !t.done && t.id != nil {
				_, _ = e.client.Rollback(cctx, &spannerpb.RollbackRequest{Session: t.session, TransactionId: t.id})
			}
			_, _ = e.client.DeleteSession(cctx, &spannerpb.DeleteSessionRequest{Name: t.session})
			cancel()
		}
	}()

	out := make([]Outcome, 0, len(steps))
	for _, st := range steps {
		t, ok := txns[st.Txn]
		if !ok {
			cctx, cancel := context.WithTimeout(ctx, callTimeout)
			s, err := e.client.CreateSession(cctx, &spannerpb.CreateSessionRequest{Database: e.database})
			cancel()
			if err != nil {
				return nil, fmt.Errorf("create session: %w", err)
			}
			t = &txnState{session: s.Name}
			txns[st.Txn] = t
		}
		cctx, cancel := context.WithTimeout(ctx, callTimeout)
		o, err := e.exec(cctx, st, t, &commitTs)
		cancel()
		if err != nil {
			return nil, fmt.Errorf("%s: %w", st, err)
		}
		out = append(out, o)
	}
	return out, nil
}

// exec runs one step. RPC failures become error outcomes; the returned error
// is reserved for harness faults.
func (e *emulator) exec(ctx context.Context, st Step, t *txnState, commitTs *[]*timestamppb.Timestamp) (Outcome, error) {
	sel := &spannerpb.TransactionSelector{Selector: &spannerpb.TransactionSelector_Id{Id: t.id}}
	switch st.Op {
	case "begin_rw", "begin_ro":
		opts := &spannerpb.TransactionOptions{Mode: &spannerpb.TransactionOptions_ReadWrite_{ReadWrite: &spannerpb.TransactionOptions_ReadWrite{}}}
		if st.Op == "begin_ro" {
			ro := &spannerpb.TransactionOptions_ReadOnly{TimestampBound: &spannerpb.TransactionOptions_ReadOnly_Strong{Strong: true}}
			if st.At != nil {
				// Clamped like the model's `roSnap`; index 0 (empty database) is not addressable here.
				n := min(*st.At, len(*commitTs))
				if n == 0 {
					return "", errors.New("begin_ro at=0 is unsupported")
				}
				ro.TimestampBound = &spannerpb.TransactionOptions_ReadOnly_ReadTimestamp{ReadTimestamp: (*commitTs)[n-1]}
			}
			opts.Mode = &spannerpb.TransactionOptions_ReadOnly_{ReadOnly: ro}
		}
		tx, err := e.client.BeginTransaction(ctx, &spannerpb.BeginTransactionRequest{Session: t.session, Options: opts})
		if err != nil {
			return errorOutcome(codeName(err)), nil
		}
		t.id, t.rw = tx.Id, st.Op == "begin_rw"
		return okOutcome(), nil

	case "read":
		rs, err := e.client.Read(ctx, &spannerpb.ReadRequest{
			Session: t.session, Transaction: sel, Table: st.Table, Columns: []string{"K", "V"}, KeySet: keySet(st.Keys),
		})
		if err != nil {
			return errorOutcome(codeName(err)), nil
		}
		rows, err := rowsOf(rs)
		if err != nil {
			return "", err
		}
		return rowsOutcome(rows), nil

	case "sql":
		params := map[string]*structpb.Value{}
		q := fmt.Sprintf("SELECT K, V FROM %s WHERE %s ORDER BY K", st.Table, predicate(st.Keys, params))
		rs, err := e.client.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{
			Session: t.session, Transaction: sel, Sql: q,
			Params: &structpb.Struct{Fields: params}, ParamTypes: paramTypes(params),
		})
		if err != nil {
			return errorOutcome(codeName(err)), nil
		}
		rows, err := rowsOf(rs)
		if err != nil {
			return "", err
		}
		return rowsOutcome(rows), nil

	case "dml_insert", "dml_update", "dml_delete":
		params := map[string]*structpb.Value{}
		var q string
		switch st.Op {
		case "dml_insert":
			params["k"], params["v"] = intValue(int64(st.Key)), intValue(st.Val)
			q = fmt.Sprintf("INSERT INTO %s (K, V) VALUES (@k, @v)", st.Table)
		case "dml_update":
			params["v"] = intValue(st.Val)
			q = fmt.Sprintf("UPDATE %s SET V = @v WHERE %s", st.Table, predicate(st.Keys, params))
		default:
			q = fmt.Sprintf("DELETE FROM %s WHERE %s", st.Table, predicate(st.Keys, params))
		}
		t.seqno++
		rs, err := e.client.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{
			Session: t.session, Transaction: sel, Sql: q, Seqno: t.seqno,
			Params: &structpb.Struct{Fields: params}, ParamTypes: paramTypes(params),
		})
		if err != nil {
			return errorOutcome(codeName(err)), nil
		}
		return countOutcome(rs.GetStats().GetRowCountExact()), nil

	case "commit":
		var muts []*spannerpb.Mutation
		if st.Txn == 0 {
			// Setup: clear the previous schedule's rows first. The model starts empty.
			for _, tbl := range tables {
				muts = append(muts, &spannerpb.Mutation{Operation: &spannerpb.Mutation_Delete_{
					Delete: &spannerpb.Mutation_Delete{Table: tbl, KeySet: &spannerpb.KeySet{All: true}}}})
			}
		}
		for _, m := range st.Muts {
			muts = append(muts, mutationProto(m))
		}
		resp, err := e.client.Commit(ctx, &spannerpb.CommitRequest{
			Session: t.session, Transaction: &spannerpb.CommitRequest_TransactionId{TransactionId: t.id}, Mutations: muts,
		})
		if err != nil {
			return errorOutcome(codeName(err)), nil
		}
		t.done = true
		if n := len(*commitTs); n > 0 && !(*commitTs)[n-1].AsTime().Before(resp.CommitTimestamp.AsTime()) {
			return "", fmt.Errorf("commit timestamp %v not after %v", resp.CommitTimestamp.AsTime(), (*commitTs)[n-1].AsTime())
		}
		*commitTs = append(*commitTs, resp.CommitTimestamp)
		return committedOutcome(len(*commitTs)), nil

	case "rollback":
		_, err := e.client.Rollback(ctx, &spannerpb.RollbackRequest{Session: t.session, TransactionId: t.id})
		if err != nil {
			return errorOutcome(codeName(err)), nil
		}
		t.done = true
		return okOutcome(), nil
	}
	return "", fmt.Errorf("unknown op %q", st.Op)
}
