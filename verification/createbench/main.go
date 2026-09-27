// Command createbench measures CreateDatabase with a DDL list against a
// running emulator: latency percentiles and throughput at several levels of
// concurrency, and the emulator's resident memory while it runs. With
// -migration it instead times one UpdateDatabaseDdl that adds indexes to a
// populated database.
//
// Each worker creates a database, then drops it (the drop is not timed), so
// at most one database per worker exists at a time.
//
// See verification/README.md.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math/big"
	"math/rand/v2"
	"os"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"cloud.google.com/go/spanner"
	database "cloud.google.com/go/spanner/admin/database/apiv1"
	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	instance "cloud.google.com/go/spanner/admin/instance/apiv1"
	instancepb "cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
)

const (
	project  = "projects/createbench"
	instName = project + "/instances/bench"
)

func main() {
	var (
		ddlPath   = flag.String("ddl", "", "DDL file: a JSON array of statements, or SQL split on ';' as the Stelaxis adapter does")
		levels    = flag.String("levels", "1,16,64", "comma-separated concurrency levels")
		creates   = flag.Int("creates", 0, "creates per level (default max(32, 8*level))")
		warmup    = flag.Int("warmup", 2, "untimed creates before the first level")
		pid       = flag.Int("pid", 0, "emulator_main pid, to sample its RSS")
		migration = flag.Int("migration", 0, "if > 0: time one UpdateDatabaseDdl adding this many of the DDL's CREATE INDEX statements to a populated database")
		rows      = flag.Int("rows", 200, "rows per table for -migration")
		repeats   = flag.Int("repeats", 3, "migrations to time with -migration")
	)
	flag.Parse()
	log.SetFlags(log.Ltime)
	ctx := context.Background()

	addr := os.Getenv("SPANNER_EMULATOR_HOST")
	if addr == "" || *ddlPath == "" {
		log.Fatal("set SPANNER_EMULATOR_HOST and -ddl")
	}
	stmts, err := readDDL(*ddlPath)
	if err != nil {
		log.Fatal(err)
	}
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Fatal(err)
	}
	opts := []option.ClientOption{option.WithGRPCConn(conn)}
	if err := createInstance(ctx, opts); err != nil {
		log.Fatal(err)
	}
	da, err := database.NewDatabaseAdminClient(ctx, opts...)
	if err != nil {
		log.Fatal(err)
	}

	if *migration > 0 {
		runMigration(ctx, da, addr, stmts, *migration, *rows, *repeats, *pid)
		return
	}

	for i := 0; i < *warmup; i++ {
		if _, err := createAndDrop(ctx, da, stmts); err != nil {
			log.Fatal(err)
		}
	}
	fmt.Printf("statements=%d\n", len(stmts))
	fmt.Printf("%-6s %-7s %-9s %-9s %-9s %-10s %-12s %-10s %-10s\n",
		"conc", "creates", "p50_ms", "p99_ms", "max_ms", "creates/s", "cpu_ms/crt", "rss_peak", "rss_end")
	for _, l := range strings.Split(*levels, ",") {
		level, err := strconv.Atoi(strings.TrimSpace(l))
		if err != nil || level < 1 {
			log.Fatalf("bad level %q", l)
		}
		n := *creates
		if n == 0 {
			n = max(32, 8*level)
		}
		cpu0 := cpuSeconds(*pid)
		sampler := startRSS(*pid)
		lat, elapsed, err := runLevel(ctx, da, stmts, level, n)
		peak, end := sampler.stop()
		cpu := cpuSeconds(*pid) - cpu0
		if err != nil {
			log.Fatal(err)
		}
		fmt.Printf("%-6d %-7d %-9.1f %-9.1f %-9.1f %-10.1f %-12.1f %-10s %-10s\n",
			level, n, pct(lat, 50), pct(lat, 99), pct(lat, 100),
			float64(n)/elapsed.Seconds(), 1000*cpu/float64(n), mib(peak), mib(end))
	}
}

func runLevel(ctx context.Context, da *database.DatabaseAdminClient, stmts []string, level, n int) ([]float64, time.Duration, error) {
	var (
		next    atomic.Int64
		mu      sync.Mutex
		lat     []float64
		firstEr error
		wg      sync.WaitGroup
	)
	start := time.Now()
	for w := 0; w < level; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for next.Add(1) <= int64(n) {
				d, err := createAndDrop(ctx, da, stmts)
				mu.Lock()
				if err != nil && firstEr == nil {
					firstEr = err
				}
				lat = append(lat, float64(d.Microseconds())/1000)
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	return lat, time.Since(start), firstEr
}

var dbSeq atomic.Int64

// createAndDrop returns the CreateDatabase latency, until the database is
// READY.
func createAndDrop(ctx context.Context, da *database.DatabaseAdminClient, stmts []string) (time.Duration, error) {
	id := fmt.Sprintf("b%d_%d", os.Getpid()%100000, dbSeq.Add(1))
	start := time.Now()
	op, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
		Parent: instName, CreateStatement: "CREATE DATABASE `" + id + "`", ExtraStatements: stmts,
	})
	if err != nil {
		return 0, fmt.Errorf("create database: %w", err)
	}
	if _, err := op.Wait(ctx); err != nil {
		return 0, fmt.Errorf("create database: %w", err)
	}
	d := time.Since(start)
	if err := da.DropDatabase(ctx, &databasepb.DropDatabaseRequest{Database: instName + "/databases/" + id}); err != nil {
		return 0, fmt.Errorf("drop database: %w", err)
	}
	return d, nil
}

func createInstance(ctx context.Context, opts []option.ClientOption) error {
	ia, err := instance.NewInstanceAdminClient(ctx, opts...)
	if err != nil {
		return err
	}
	for attempt := 0; ; attempt++ {
		op, err := ia.CreateInstance(ctx, &instancepb.CreateInstanceRequest{
			Parent: project, InstanceId: "bench",
			Instance: &instancepb.Instance{
				Config: project + "/instanceConfigs/emulator-config", DisplayName: "bench", NodeCount: 1,
			},
		})
		if err == nil {
			_, err = op.Wait(ctx)
		}
		if err == nil || status.Code(err) == codes.AlreadyExists {
			return nil
		}
		if status.Code(err) != codes.Unavailable || attempt > 100 {
			return fmt.Errorf("create instance: %w", err)
		}
		time.Sleep(100 * time.Millisecond) // emulator still starting
	}
}

// runMigration creates a database with every statement except the first
// `count` CREATE INDEX statements, fills each table with rows, and times one
// UpdateDatabaseDdl that creates those indexes.
func runMigration(ctx context.Context, da *database.DatabaseAdminClient, addr string, stmts []string, count, rows, repeats, pid int) {
	var base, indexes []string
	for _, s := range stmts {
		if len(indexes) < count && createIndexRE.MatchString(s) {
			indexes = append(indexes, s)
		} else {
			base = append(base, s)
		}
	}
	if len(indexes) < count {
		log.Fatalf("only %d CREATE INDEX statements", len(indexes))
	}
	fmt.Printf("base_statements=%d index_statements=%d rows_per_table=%d\n", len(base), len(indexes), rows)
	for r := 0; r < repeats; r++ {
		id := fmt.Sprintf("m%d_%d", os.Getpid()%100000, r)
		name := instName + "/databases/" + id
		op, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
			Parent: instName, CreateStatement: "CREATE DATABASE `" + id + "`", ExtraStatements: base,
		})
		if err == nil {
			_, err = op.Wait(ctx)
		}
		if err != nil {
			log.Fatalf("create database: %v", err)
		}
		inserted, err := populate(ctx, addr, name, rows)
		if err != nil {
			log.Fatalf("populate: %v", err)
		}
		cpu0 := cpuSeconds(pid)
		sampler := startRSS(pid)
		start := time.Now()
		uop, err := da.UpdateDatabaseDdl(ctx, &databasepb.UpdateDatabaseDdlRequest{Database: name, Statements: indexes})
		if err == nil {
			err = uop.Wait(ctx)
		}
		d := time.Since(start)
		peak, _ := sampler.stop()
		cpu := cpuSeconds(pid) - cpu0
		if err != nil {
			log.Fatalf("update ddl: %v", err)
		}
		fmt.Printf("migration %d: rows=%d ddl_ms=%.1f cpu_ms=%.0f rss_peak=%s\n", r, inserted, float64(d.Microseconds())/1000, 1000*cpu, mib(peak))
		if err := da.DropDatabase(ctx, &databasepb.DropDatabaseRequest{Database: name}); err != nil {
			log.Fatalf("drop database: %v", err)
		}
	}
}

var (
	checkInRE = regexp.MustCompile(`(?i)\b(\w+)\s+IN\s*\(\s*'([^']*)'`)
	lengthRE  = regexp.MustCompile(`\((\d+)\)$`)
)

var createIndexRE = regexp.MustCompile(`(?is)^\s*CREATE\s+(UNIQUE\s+)?(NULL_FILTERED\s+)?INDEX\b`)

type column struct {
	name, typ string
	nullable  bool
	generated bool
}

type fkRef struct {
	cols, refCols []string
	refTable      string
}

// populate inserts `rows` rows into every table. Tables are filled parents
// first; foreign-key and interleaving columns copy the referenced row's
// values, other columns get random values of their type.
func populate(ctx context.Context, addr, name string, rows int) (int, error) {
	client, err := spanner.NewClient(ctx, name, option.WithEndpoint(addr),
		option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials())),
		option.WithoutAuthentication())
	if err != nil {
		return 0, err
	}
	defer client.Close()
	ro := client.ReadOnlyTransaction()
	defer ro.Close()

	cols := map[string][]column{}
	var order []string
	err = query(ctx, ro, `SELECT TABLE_NAME, COLUMN_NAME, SPANNER_TYPE, IS_NULLABLE, IS_GENERATED
		FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = '' AND TABLE_NAME IN
		  (SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = '' AND TABLE_TYPE = 'BASE TABLE')
		ORDER BY TABLE_NAME, ORDINAL_POSITION`,
		func(r *spanner.Row) error {
			var t, c, typ, nullable, gen string
			if err := r.Columns(&t, &c, &typ, &nullable, &gen); err != nil {
				return err
			}
			if _, ok := cols[t]; !ok {
				order = append(order, t)
			}
			cols[t] = append(cols[t], column{c, typ, nullable == "YES", gen != "NEVER"})
			return nil
		})
	if err != nil {
		return 0, err
	}
	parents := map[string]string{}
	err = query(ctx, ro, `SELECT TABLE_NAME, PARENT_TABLE_NAME FROM INFORMATION_SCHEMA.TABLES
		WHERE TABLE_SCHEMA = '' AND PARENT_TABLE_NAME IS NOT NULL`, func(r *spanner.Row) error {
		var t, p string
		if err := r.Columns(&t, &p); err != nil {
			return err
		}
		parents[t] = p
		return nil
	})
	if err != nil {
		return 0, err
	}
	fks := map[string][]fkRef{}
	fkIndex := map[string]int{} // table/constraint -> index in fks[table]
	err = query(ctx, ro, `SELECT kcu.TABLE_NAME, kcu.CONSTRAINT_NAME, kcu.COLUMN_NAME, rk.TABLE_NAME, rk.COLUMN_NAME
		FROM INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS rc
		JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE kcu
		  ON kcu.CONSTRAINT_NAME = rc.CONSTRAINT_NAME AND kcu.CONSTRAINT_SCHEMA = rc.CONSTRAINT_SCHEMA
		JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE rk
		  ON rk.CONSTRAINT_NAME = rc.UNIQUE_CONSTRAINT_NAME AND rk.CONSTRAINT_SCHEMA = rc.UNIQUE_CONSTRAINT_SCHEMA
		 AND rk.ORDINAL_POSITION = kcu.POSITION_IN_UNIQUE_CONSTRAINT
		ORDER BY kcu.TABLE_NAME, kcu.CONSTRAINT_NAME, kcu.ORDINAL_POSITION`, func(r *spanner.Row) error {
		var t, c, col, rt, rc string
		if err := r.Columns(&t, &c, &col, &rt, &rc); err != nil {
			return err
		}
		i, ok := fkIndex[t+"/"+c]
		if !ok {
			i = len(fks[t])
			fkIndex[t+"/"+c] = i
			fks[t] = append(fks[t], fkRef{refTable: rt})
		}
		fks[t][i].cols = append(fks[t][i].cols, col)
		fks[t][i].refCols = append(fks[t][i].refCols, rc)
		return nil
	})
	if err != nil {
		return 0, err
	}

	// A column constrained by CHECK(col IN ('v', ...)) gets 'v'.
	choices := map[string]string{} // table/column -> value
	err = query(ctx, ro, `SELECT tc.TABLE_NAME, cc.CHECK_CLAUSE
		FROM INFORMATION_SCHEMA.CHECK_CONSTRAINTS cc
		JOIN INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc
		  ON tc.CONSTRAINT_NAME = cc.CONSTRAINT_NAME AND tc.CONSTRAINT_SCHEMA = cc.CONSTRAINT_SCHEMA`,
		func(r *spanner.Row) error {
			var t, clause string
			if err := r.Columns(&t, &clause); err != nil {
				return err
			}
			for _, m := range checkInRE.FindAllStringSubmatch(clause, -1) {
				choices[t+"/"+m[1]] = m[2]
			}
			return nil
		})
	if err != nil {
		return 0, err
	}

	// Values written per table, for children to copy.
	written := map[string][]map[string]any{}
	done := map[string]bool{}
	inserted := 0
	rng := rand.New(rand.NewPCG(1, 2))
	// force: no table was ready in the last pass (a foreign-key cycle), so
	// fill the first remaining one, with NULL in foreign keys to tables that
	// have no rows yet.
	force := false
	for len(done) < len(order) {
		progress := false
		for _, t := range order {
			if done[t] {
				continue
			}
			deps := []string{}
			if p, ok := parents[t]; ok {
				deps = append(deps, p)
			}
			for _, f := range fks[t] {
				if f.refTable != t {
					deps = append(deps, f.refTable)
				}
			}
			ready := true
			for _, d := range deps {
				if !done[d] {
					ready = false
				}
			}
			if !ready && !(force && canBreakCycle(t, parents, fks, cols, done)) {
				continue
			}
			force = false
			var ms []*spanner.Mutation
			var vals []map[string]any
			for i := 0; i < rows; i++ {
				v := map[string]any{}
				if p, ok := parents[t]; ok && len(written[p]) > 0 {
					prow := written[p][i%len(written[p])]
					for k, x := range prow {
						if hasColumn(cols[t], k) {
							v[k] = x
						}
					}
				}
				for _, f := range fks[t] {
					ref := written[f.refTable]
					if len(ref) == 0 || f.refTable == t {
						// NULL in any column exempts the row from the key.
						for _, c := range f.cols {
							if _, set := v[c]; !set && isNullable(cols[t], c) {
								v[c] = nil
							}
						}
						continue
					}
					rrow := ref[rng.IntN(len(ref))]
					for j, c := range f.cols {
						if _, set := v[c]; !set {
							v[c] = rrow[f.refCols[j]]
						}
					}
				}
				var names []string
				var values []any
				for _, c := range cols[t] {
					if c.generated {
						continue
					}
					x, ok := v[c.name]
					if choice, isChoice := choices[t+"/"+c.name]; !ok && isChoice {
						x, ok = choice, true
						v[c.name] = x
					}
					if !ok {
						x = randomValue(rng, c.typ)
						v[c.name] = x
					}
					names = append(names, c.name)
					values = append(values, x)
				}
				ms = append(ms, spanner.InsertOrUpdate(t, names, values))
				vals = append(vals, v)
			}
			if _, err := client.Apply(ctx, ms); err != nil {
				log.Printf("populate %s: %v (table left empty)", t, err)
			} else {
				written[t] = vals
				inserted += len(ms)
			}
			done[t] = true
			progress = true
		}
		if !progress {
			if force {
				break // nothing left that can be filled
			}
			force = true
		}
	}
	return inserted, nil
}

// canBreakCycle reports whether t can be filled before the tables its foreign
// keys reference: its parent is filled, and each such key has a nullable
// column.
func canBreakCycle(t string, parents map[string]string, fks map[string][]fkRef, cols map[string][]column, done map[string]bool) bool {
	if p, ok := parents[t]; ok && !done[p] {
		return false
	}
	for _, f := range fks[t] {
		if done[f.refTable] || f.refTable == t {
			continue
		}
		nullable := false
		for _, c := range f.cols {
			nullable = nullable || isNullable(cols[t], c)
		}
		if !nullable {
			return false
		}
	}
	return true
}

func isNullable(cs []column, name string) bool {
	for _, c := range cs {
		if c.name == name {
			return c.nullable
		}
	}
	return false
}

func hasColumn(cs []column, name string) bool {
	for _, c := range cs {
		if c.name == name {
			return true
		}
	}
	return false
}

func randomValue(rng *rand.Rand, typ string) any {
	switch {
	case strings.HasPrefix(typ, "ARRAY"):
		return nil
	case typ == "INT64":
		return rng.Int64()
	case typ == "FLOAT64" || typ == "FLOAT32":
		return rng.Float64()
	case typ == "BOOL":
		return rng.IntN(2) == 0
	case typ == "TIMESTAMP":
		return time.Unix(1_700_000_000+rng.Int64N(100_000_000), 0).UTC()
	case typ == "DATE":
		return time.Unix(1_700_000_000+rng.Int64N(100_000_000), 0).UTC().Format("2006-01-02")
	case typ == "UUID":
		return fmt.Sprintf("%08x-%04x-4%03x-8%03x-%012x", rng.Uint32(), rng.IntN(1<<16), rng.IntN(1<<12), rng.IntN(1<<12), rng.Int64N(1<<48))
	case typ == "JSON":
		return spanner.NullJSON{Value: map[string]any{"k": rng.IntN(1000)}, Valid: true}
	case typ == "NUMERIC":
		return big.NewRat(rng.Int64N(1_000_000), 100)
	case strings.HasPrefix(typ, "BYTES"):
		return []byte(fmt.Sprintf("b%d", rng.Int64()))
	default: // STRING(n)
		str := fmt.Sprintf("s%d", rng.Int64N(1_000_000_000))
		if m := lengthRE.FindStringSubmatch(typ); m != nil {
			if n, _ := strconv.Atoi(m[1]); n < len(str) {
				str = str[len(str)-n:]
			}
		}
		return str
	}
}

func query(ctx context.Context, ro *spanner.ReadOnlyTransaction, sql string, f func(*spanner.Row) error) error {
	it := ro.Query(ctx, spanner.Statement{SQL: sql})
	defer it.Stop()
	for {
		r, err := it.Next()
		if err == iterator.Done {
			return nil
		}
		if err != nil {
			return err
		}
		if err := f(r); err != nil {
			return err
		}
	}
}

// readDDL reads a JSON array of statements, or SQL that it splits the way
// the Stelaxis Spanner adapter's structure_load does: on ';', trimmed, with
// foreign keys that reference a table defined later moved into trailing
// ALTER TABLE statements.
func readDDL(path string) ([]string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if strings.HasSuffix(path, ".json") {
		var stmts []string
		return stmts, json.Unmarshal(b, &stmts)
	}
	var stmts []string
	for _, s := range strings.Split(string(b), ";") {
		if s = strings.TrimSpace(s); s != "" {
			stmts = append(stmts, s)
		}
	}
	return deferForwardFKs(stmts), nil
}

var (
	createTableRE = regexp.MustCompile(`(?i)\ACREATE TABLE (\S+)`)
	fkLineRE      = regexp.MustCompile(`(?i)^\s*CONSTRAINT\s+(\S+)\s+FOREIGN\s+KEY\(([^)]+)\)\s+REFERENCES\s+(\S+)\(([^)]+)\),?\s*$`)
)

func deferForwardFKs(stmts []string) []string {
	var out, alters []string
	defined := map[string]bool{}
	for _, s := range stmts {
		m := createTableRE.FindStringSubmatch(s)
		if m == nil {
			out = append(out, s)
			continue
		}
		var kept []string
		for _, line := range strings.Split(s, "\n") {
			f := fkLineRE.FindStringSubmatch(line)
			if f != nil && !defined[f[3]] {
				alters = append(alters, fmt.Sprintf("ALTER TABLE %s ADD CONSTRAINT %s FOREIGN KEY(%s) REFERENCES %s(%s)", m[1], f[1], f[2], f[3], f[4]))
				continue
			}
			kept = append(kept, line)
		}
		out = append(out, strings.Join(kept, "\n"))
		defined[m[1]] = true
	}
	return append(out, alters...)
}

func pct(xs []float64, p float64) float64 {
	if len(xs) == 0 {
		return 0
	}
	s := append([]float64(nil), xs...)
	sort.Float64s(s)
	i := int(float64(len(s))*p/100+0.5) - 1
	return s[min(max(i, 0), len(s)-1)]
}

type rssSampler struct {
	stopc chan struct{}
	done  chan struct{}
	peak  int64
	end   int64
}

// startRSS samples the resident set size of pid every 20 ms.
func startRSS(pid int) *rssSampler {
	s := &rssSampler{stopc: make(chan struct{}), done: make(chan struct{})}
	go func() {
		defer close(s.done)
		for {
			if pid > 0 {
				if kb := rssKB(pid); kb > s.peak {
					s.peak = kb
				}
			}
			select {
			case <-s.stopc:
				if pid > 0 {
					s.end = rssKB(pid)
				}
				return
			case <-time.After(20 * time.Millisecond):
			}
		}
	}()
	return s
}

func (s *rssSampler) stop() (peak, end int64) {
	close(s.stopc)
	<-s.done
	return s.peak, s.end
}

func rssKB(pid int) int64 {
	out, err := exec.Command("ps", "-o", "rss=", "-p", strconv.Itoa(pid)).Output()
	if err != nil {
		return 0
	}
	kb, _ := strconv.ParseInt(strings.TrimSpace(string(out)), 10, 64)
	return kb
}

// cpuSeconds returns the user plus system CPU time pid has used, including
// the drops, which are cheap.
func cpuSeconds(pid int) float64 {
	if pid == 0 {
		return 0
	}
	out, err := exec.Command("ps", "-o", "time=", "-p", strconv.Itoa(pid)).Output()
	if err != nil {
		return 0
	}
	// [[DD-]HH:]MM:SS.ss
	var secs float64
	for _, part := range strings.Split(strings.Replace(strings.TrimSpace(string(out)), "-", ":", 1), ":") {
		v, _ := strconv.ParseFloat(part, 64)
		secs = secs*60 + v
	}
	return secs
}

func mib(kb int64) string {
	if kb == 0 {
		return "-"
	}
	return fmt.Sprintf("%dMiB", kb/1024)
}
