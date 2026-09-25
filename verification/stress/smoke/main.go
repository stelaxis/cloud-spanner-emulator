// Command smoke applies a schema file (such as Stelaxis's structure.sql) to a
// persistent emulator, fills every table it can, kills the emulator with
// SIGKILL, restarts it on the same data directory and checks that the schema
// and every row came back unchanged, and that sequences continue without
// reissuing a value. See verification/README.md.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/exec"
	"regexp"
	"slices"
	"strings"
	"syscall"
	"time"

	"cloud.google.com/go/spanner"
	database "cloud.google.com/go/spanner/admin/database/apiv1"
	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	instance "cloud.google.com/go/spanner/admin/instance/apiv1"
	instancepb "cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

const (
	project = "projects/smoke"
	inst    = project + "/instances/smoke"
	dbName  = inst + "/databases/app"
)

func main() {
	binary := flag.String("emulator-binary", "", "emulator_main to run")
	schemaPath := flag.String("schema", "", "DDL file; statements end with ';'")
	port := flag.Int("port", 19220, "emulator port")
	rows := flag.Int("rows", 3, "rows per table")
	migration := flag.Bool("migrate", false, "after filling, time one schema change creating an index on every table")
	flag.Parse()
	log.SetFlags(log.Ltime)
	if err := run(*binary, *schemaPath, *port, *rows, *migration); err != nil {
		log.Fatal(err)
	}
}

type emulator struct {
	cmd  *exec.Cmd
	args []string
}

func (e *emulator) start(binary string) error {
	e.cmd = exec.Command(binary, e.args...)
	e.cmd.Stderr = os.Stderr
	return e.cmd.Start()
}

func run(binary, schemaPath string, port, perTable int, migration bool) error {
	ctx := context.Background()
	schema, err := os.ReadFile(schemaPath)
	if err != nil {
		return err
	}
	// Foreign keys can form cycles with interleaving (a parent referencing its
	// child), so they are added once every table exists.
	var statements, foreignKeys []string
	fk := regexp.MustCompile(`^\s*(CONSTRAINT \w+ FOREIGN KEY.*?),?\s*$`)
	for _, s := range strings.Split(string(schema), ";") {
		if s = strings.TrimSpace(s); s == "" {
			continue
		}
		if m := regexp.MustCompile(`^CREATE TABLE (\w+)`).FindStringSubmatch(s); m != nil {
			var kept []string
			for _, line := range strings.Split(s, "\n") {
				if c := fk.FindStringSubmatch(line); c != nil {
					foreignKeys = append(foreignKeys, fmt.Sprintf("ALTER TABLE %s ADD %s", m[1], c[1]))
					continue
				}
				kept = append(kept, line)
			}
			s = strings.Join(kept, "\n")
		}
		statements = append(statements, s)
	}
	statements = append(statements, foreignKeys...)
	dir, err := os.MkdirTemp("", "smoke-data-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)
	addr := fmt.Sprintf("localhost:%d", port)
	emu := &emulator{args: []string{"--host_port", addr, "--data_dir", dir}}
	if err := emu.start(binary); err != nil {
		return err
	}
	defer func() { _ = emu.cmd.Process.Kill(); _ = emu.cmd.Wait() }()
	opts := []option.ClientOption{
		option.WithEndpoint(addr),
		option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials())),
		option.WithoutAuthentication(),
	}
	if err := create(ctx, opts, statements); err != nil {
		return err
	}
	log.Printf("applied %d statements from %s", len(statements), schemaPath)
	client, err := spanner.NewClientWithConfig(ctx, dbName, spanner.ClientConfig{DisableNativeMetrics: true}, opts...)
	if err != nil {
		return err
	}
	tables, err := columns(ctx, client)
	if err != nil {
		return err
	}
	filled, err := fill(ctx, client, tables, perTable)
	if err != nil {
		return err
	}
	log.Printf("filled %d of %d tables", filled, len(tables))
	if filled < len(tables)*3/4 {
		return fmt.Errorf("only %d of %d tables could be filled", filled, len(tables))
	}
	if migration {
		n, took, err := migrate(ctx, opts, tables)
		if err != nil {
			return fmt.Errorf("migration: %w", err)
		}
		log.Printf("migration: %d CREATE INDEX statements in one schema change took %v", n, took.Round(time.Millisecond))
	}
	ddl, err := getDDL(ctx, opts)
	if err != nil {
		return err
	}
	before, err := dump(ctx, client, tables)
	if err != nil {
		return err
	}
	seqIDs, err := sequenceKeys(ctx, client, tables)
	if err != nil {
		return err
	}
	client.Close()

	if err := emu.cmd.Process.Signal(syscall.SIGKILL); err != nil {
		return err
	}
	_ = emu.cmd.Wait()
	if ws := emu.cmd.ProcessState.Sys().(syscall.WaitStatus); !ws.Signaled() || ws.Signal() != syscall.SIGKILL {
		return fmt.Errorf("SIGKILL not confirmed: %v", emu.cmd.ProcessState)
	}
	log.Printf("killed the emulator with SIGKILL (%d rows in %d tables)", countRows(before), len(before))
	started := time.Now()
	if err := emu.start(binary); err != nil {
		return err
	}
	client, err = spanner.NewClientWithConfig(ctx, dbName, spanner.ClientConfig{DisableNativeMetrics: true}, opts...)
	if err != nil {
		return err
	}
	defer client.Close()
	var after map[string][]string
	for deadline := time.Now().Add(60 * time.Second); ; {
		if after, err = dump(ctx, client, tables); err == nil || time.Now().After(deadline) {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if err != nil {
		return fmt.Errorf("after restart: %w", err)
	}
	log.Printf("restarted and read back in %v", time.Since(started).Round(time.Millisecond))
	ddlAfter, err := getDDL(ctx, opts)
	if err != nil {
		return err
	}
	if !slices.Equal(ddl, ddlAfter) {
		return fmt.Errorf("schema changed across the restart")
	}
	for table, rows := range before {
		if !slices.Equal(rows, after[table]) {
			return fmt.Errorf("table %s changed across the restart:\nbefore %v\nafter  %v", table, rows, after[table])
		}
	}
	// Sequences continue without handing out a value twice.
	added, err := fill(ctx, client, tables, perTable+1)
	if err != nil {
		return err
	}
	seqAfter, err := sequenceKeys(ctx, client, tables)
	if err != nil {
		return err
	}
	for table, keys := range seqIDs {
		if len(seqAfter[table]) <= len(keys) {
			return fmt.Errorf("no new row in %s after the restart", table)
		}
		seen := map[string]bool{}
		for _, k := range seqAfter[table] {
			if seen[k] {
				return fmt.Errorf("sequence value %s reissued in %s", k, table)
			}
			seen[k] = true
		}
	}
	fmt.Printf("smoke: statements=%d tables=%d filled=%d rows=%d ddl_statements=%d identical_after_sigkill=true sequence_tables=%d refilled=%d\n",
		len(statements), len(tables), filled, countRows(before), len(ddl), len(seqIDs), added)
	return nil
}

func create(ctx context.Context, opts []option.ClientOption, statements []string) error {
	ia, err := instance.NewInstanceAdminClient(ctx, opts...)
	if err != nil {
		return err
	}
	defer ia.Close()
	var op *instance.CreateInstanceOperation
	for attempt := 0; ; attempt++ {
		op, err = ia.CreateInstance(ctx, &instancepb.CreateInstanceRequest{Parent: project, InstanceId: "smoke",
			Instance: &instancepb.Instance{Config: project + "/instanceConfigs/emulator-config", DisplayName: "smoke", NodeCount: 1}})
		if err == nil || attempt > 100 {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if err != nil {
		return err
	}
	if _, err := op.Wait(ctx); err != nil {
		return err
	}
	da, err := database.NewDatabaseAdminClient(ctx, opts...)
	if err != nil {
		return err
	}
	defer da.Close()
	dop, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{Parent: inst, CreateStatement: "CREATE DATABASE app"})
	if err != nil {
		return err
	}
	if _, err := dop.Wait(ctx); err != nil {
		return err
	}
	// One statement per schema change, as migrations would. A statement that
	// refers to a table defined later in the file is retried after it.
	pending := statements
	for len(pending) > 0 {
		var failed []string
		var lastErr error
		for _, statement := range pending {
			uop, err := da.UpdateDatabaseDdl(ctx, &databasepb.UpdateDatabaseDdlRequest{Database: dbName, Statements: []string{statement}})
			if err == nil {
				err = uop.Wait(ctx)
			}
			if err != nil {
				failed, lastErr = append(failed, statement), err
			}
		}
		if len(failed) == len(pending) {
			return fmt.Errorf("%d statements cannot be applied: %w", len(failed), lastErr)
		}
		pending = failed
	}
	return nil
}

func getDDL(ctx context.Context, opts []option.ClientOption) ([]string, error) {
	da, err := database.NewDatabaseAdminClient(ctx, opts...)
	if err != nil {
		return nil, err
	}
	defer da.Close()
	resp, err := da.GetDatabaseDdl(ctx, &databasepb.GetDatabaseDdlRequest{Database: dbName})
	if err != nil {
		return nil, err
	}
	return resp.Statements, nil
}

type column struct {
	name, typ string
	nullable  bool
	omitted   bool // generated or defaulted: left to the emulator
	key       bool
}

type table struct {
	name    string
	columns []column
}

func columns(ctx context.Context, client *spanner.Client) ([]table, error) {
	stmt := spanner.Statement{SQL: `
		SELECT c.TABLE_NAME, c.COLUMN_NAME, c.SPANNER_TYPE, c.IS_NULLABLE = 'YES',
		       c.IS_GENERATED = 'ALWAYS' OR c.COLUMN_DEFAULT IS NOT NULL,
		       EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.INDEX_COLUMNS k
		               WHERE k.TABLE_NAME = c.TABLE_NAME AND k.COLUMN_NAME = c.COLUMN_NAME
		                 AND k.INDEX_TYPE = 'PRIMARY_KEY' AND k.TABLE_SCHEMA = '')
		FROM INFORMATION_SCHEMA.COLUMNS c
		JOIN INFORMATION_SCHEMA.TABLES t USING (TABLE_SCHEMA, TABLE_NAME)
		WHERE c.TABLE_SCHEMA = '' AND t.TABLE_TYPE = 'BASE TABLE'
		ORDER BY c.TABLE_NAME, c.ORDINAL_POSITION`}
	var tables []table
	err := client.Single().Query(ctx, stmt).Do(func(row *spanner.Row) error {
		var t string
		var c column
		if err := row.Columns(&t, &c.name, &c.typ, &c.nullable, &c.omitted, &c.key); err != nil {
			return err
		}
		if len(tables) == 0 || tables[len(tables)-1].name != t {
			tables = append(tables, table{name: t})
		}
		tables[len(tables)-1].columns = append(tables[len(tables)-1].columns, c)
		return nil
	})
	return tables, err
}

var uuids = []string{
	"00000000-0000-4000-8000-000000000001",
	"00000000-0000-4000-8000-000000000002",
	"00000000-0000-4000-8000-000000000003",
	"00000000-0000-4000-8000-000000000004",
}

// literal is a value of `typ` for row r. UUID columns named tenant_id share
// one value and other UUIDs follow the row, so foreign keys between rows
// with the same index line up. Strings favour values that CHECK constraints
// on kind/state columns accept.
func literal(c column, r int, nullOK bool) string {
	if nullOK && c.nullable && !c.key {
		return "NULL"
	}
	typ := c.typ
	if strings.HasPrefix(typ, "ARRAY<") {
		elem := column{name: c.name, typ: strings.TrimSuffix(strings.TrimPrefix(typ, "ARRAY<"), ">")}
		return fmt.Sprintf("[%s, %s]", literal(elem, r, false), literal(elem, r+1, false))
	}
	switch {
	case typ == "UUID":
		if c.name == "tenant_id" {
			return fmt.Sprintf("CAST('%s' AS UUID)", uuids[0])
		}
		return fmt.Sprintf("CAST('00000000-0000-4000-8000-%012d' AS UUID)", r+1)
	case strings.HasPrefix(typ, "STRING"):
		switch c.name {
		case "kind":
			return "'root'"
		case "state":
			return "'running'"
		}
		return fmt.Sprintf("'%s-%d'", c.name, r)
	case typ == "INT64":
		return fmt.Sprint(r + 1)
	case typ == "BOOL":
		return "TRUE"
	case typ == "FLOAT64":
		return fmt.Sprintf("%d.5", r)
	case typ == "NUMERIC":
		return fmt.Sprintf("NUMERIC '%d.25'", r+1)
	case typ == "JSON":
		return fmt.Sprintf(`JSON '{"row": %d, "tags": ["a", "b"]}'`, r)
	case typ == "DATE":
		return fmt.Sprintf("DATE '2026-01-%02d'", r+1)
	case typ == "TIMESTAMP":
		return fmt.Sprintf("TIMESTAMP '2026-01-01T00:00:%02d.123456Z'", r)
	case strings.HasPrefix(typ, "BYTES"):
		return fmt.Sprintf("b'row-%d'", r)
	}
	return "NULL"
}

// fill inserts up to `perTable` rows into every table, in as many passes as
// foreign keys and interleaving need. Returns how many tables have rows.
func insertSQL(t table, r int, nullOK bool) string {
	var names, values []string
	for _, c := range t.columns {
		if c.omitted {
			continue
		}
		names = append(names, c.name)
		values = append(values, literal(c, r, nullOK))
	}
	return fmt.Sprintf("INSERT OR IGNORE INTO %s (%s) VALUES (%s)", t.name, strings.Join(names, ", "), strings.Join(values, ", "))
}

// fill inserts up to `perTable` rows into every table, in as many passes as
// foreign keys and interleaving need: one row at a time until one succeeds,
// then in batches of 100 like it. Returns how many tables have rows.
func fill(ctx context.Context, client *spanner.Client, tables []table, perTable int) (int, error) {
	done := map[string]int{}
	nulls := map[string]bool{} // whether a table's rows need NULL in nullable columns
	for pass := 0; pass < len(tables); pass++ {
		progress := false
		for _, t := range tables {
			for done[t.name] < perTable {
				r := done[t.name]
				if r > 0 {
					end := min(r+100, perTable)
					var stmts []spanner.Statement
					for i := r; i < end; i++ {
						stmts = append(stmts, spanner.Statement{SQL: insertSQL(t, i, nulls[t.name])})
					}
					_, err := client.ReadWriteTransaction(ctx, func(ctx context.Context, txn *spanner.ReadWriteTransaction) error {
						_, err := txn.BatchUpdate(ctx, stmts)
						return err
					})
					if err == nil {
						done[t.name], progress = end, true
						continue
					}
				}
				ok := false
				for _, nullOK := range []bool{false, true} {
					sql := insertSQL(t, r, nullOK)
					_, err := client.ReadWriteTransaction(ctx, func(ctx context.Context, txn *spanner.ReadWriteTransaction) error {
						_, err := txn.Update(ctx, spanner.Statement{SQL: sql})
						return err
					})
					if err == nil {
						ok, nulls[t.name] = true, nullOK
						break
					}
				}
				if !ok {
					break
				}
				done[t.name], progress = r+1, true
			}
		}
		if !progress {
			break
		}
	}
	filled := 0
	for _, t := range tables {
		if done[t.name] > 0 {
			filled++
		}
	}
	return filled, nil
}

// migrate applies one schema change creating an index on a non-key column of
// every table that has one, like a migration on a populated database, and
// returns how long it took.
func migrate(ctx context.Context, opts []option.ClientOption, tables []table) (int, time.Duration, error) {
	indexable := map[string]bool{"INT64": true, "BOOL": true, "DATE": true, "TIMESTAMP": true, "UUID": true, "STRING(MAX)": true}
	var statements []string
	for _, t := range tables {
		for _, c := range t.columns {
			if !c.key && !c.omitted && indexable[c.typ] {
				statements = append(statements, fmt.Sprintf("CREATE INDEX smoke_%s ON %s(%s)", t.name, t.name, c.name))
				break
			}
		}
	}
	da, err := database.NewDatabaseAdminClient(ctx, opts...)
	if err != nil {
		return 0, 0, err
	}
	defer da.Close()
	start := time.Now()
	op, err := da.UpdateDatabaseDdl(ctx, &databasepb.UpdateDatabaseDdlRequest{Database: dbName, Statements: statements})
	if err == nil {
		err = op.Wait(ctx)
	}
	return len(statements), time.Since(start), err
}

func dump(ctx context.Context, client *spanner.Client, tables []table) (map[string][]string, error) {
	out := map[string][]string{}
	txn := client.ReadOnlyTransaction()
	defer txn.Close()
	for _, t := range tables {
		var rows []string
		err := txn.Query(ctx, spanner.Statement{SQL: "SELECT * FROM " + t.name}).Do(func(row *spanner.Row) error {
			var values []string
			for i := 0; i < row.Size(); i++ {
				var v spanner.GenericColumnValue
				if err := row.Column(i, &v); err != nil {
					return err
				}
				values = append(values, v.Value.String())
			}
			rows = append(rows, strings.Join(values, "|"))
			return nil
		})
		if err != nil {
			return nil, err
		}
		slices.Sort(rows)
		out[t.name] = rows
	}
	return out, nil
}

// sequenceKeys returns, for each table with a defaulted INT64 key column, its
// key values.
func sequenceKeys(ctx context.Context, client *spanner.Client, tables []table) (map[string][]string, error) {
	out := map[string][]string{}
	for _, t := range tables {
		for _, c := range t.columns {
			if !c.key || !c.omitted || c.typ != "INT64" {
				continue
			}
			var keys []string
			err := client.Single().Query(ctx, spanner.Statement{SQL: fmt.Sprintf("SELECT CAST(%s AS STRING) FROM %s", c.name, t.name)}).Do(func(row *spanner.Row) error {
				var k string
				if err := row.Column(0, &k); err != nil {
					return err
				}
				keys = append(keys, k)
				return nil
			})
			if err != nil {
				return nil, err
			}
			out[t.name] = keys
		}
	}
	return out, nil
}

func countRows(d map[string][]string) int {
	n := 0
	for _, rows := range d {
		n += len(rows)
	}
	return n
}
