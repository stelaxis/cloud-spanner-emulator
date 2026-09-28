package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	database "cloud.google.com/go/spanner/admin/database/apiv1"
	databasepb "cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	spannerapi "cloud.google.com/go/spanner/apiv1"
	"cloud.google.com/go/spanner/apiv1/spannerpb"
	"google.golang.org/api/option"
)

// runEphemeral creates one database from the DDL, fills every table with
// `rows` rows, and then times CreateSession with the label
// emulator-ephemeral=true, which copies the database, at each concurrency
// level; each session is deleted (untimed) right after. It then holds `hold`
// ephemeral sessions open at once to measure the resident memory per copy.
func runEphemeral(ctx context.Context, da *database.DatabaseAdminClient, opts []option.ClientOption,
	addr string, stmts []string, rows int, levels []int, creates, hold, pid int) {
	id := fmt.Sprintf("e%d", os.Getpid()%100000)
	name := instName + "/databases/" + id
	op, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
		Parent: instName, CreateStatement: "CREATE DATABASE `" + id + "`", ExtraStatements: stmts,
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
	sc, err := spannerapi.NewClient(ctx, opts...)
	if err != nil {
		log.Fatal(err)
	}
	defer sc.Close()
	create := func() (string, time.Duration, error) {
		start := time.Now()
		s, err := sc.CreateSession(ctx, &spannerpb.CreateSessionRequest{
			Database: name,
			Session:  &spannerpb.Session{Labels: map[string]string{"emulator-ephemeral": "true"}},
		})
		if err != nil {
			return "", 0, fmt.Errorf("create session: %w", err)
		}
		return s.Name, time.Since(start), nil
	}
	remove := func(session string) error {
		return sc.DeleteSession(ctx, &spannerpb.DeleteSessionRequest{Name: session})
	}
	// The copy must hold the rows.
	s, _, err := create()
	if err != nil {
		log.Fatal(err)
	}
	if err := checkCopy(ctx, sc, s); err != nil {
		log.Fatal(err)
	}
	if err := remove(s); err != nil {
		log.Fatal(err)
	}

	fmt.Printf("statements=%d rows=%d (%d per table)\n", len(stmts), inserted, rows)
	fmt.Printf("%-6s %-9s %-9s %-9s %-9s %-11s %-12s %-10s\n",
		"conc", "sessions", "p50_ms", "p99_ms", "max_ms", "sessions/s", "cpu_ms/copy", "rss_peak")
	for _, level := range levels {
		n := creates
		if n == 0 {
			n = max(32, 8*level)
		}
		var (
			next    atomic.Int64
			mu      sync.Mutex
			lat     []float64
			firstEr error
			wg      sync.WaitGroup
		)
		cpu0 := cpuSeconds(pid)
		sampler := startRSS(pid)
		start := time.Now()
		for w := 0; w < level; w++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for next.Add(1) <= int64(n) {
					s, d, err := create()
					if err == nil {
						err = remove(s)
					}
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
		elapsed := time.Since(start)
		peak, _ := sampler.stop()
		cpu := cpuSeconds(pid) - cpu0
		if firstEr != nil {
			log.Fatal(firstEr)
		}
		fmt.Printf("%-6d %-9d %-9.1f %-9.1f %-9.1f %-11.1f %-12.1f %-10s\n",
			level, n, pct(lat, 50), pct(lat, 99), pct(lat, 100),
			float64(n)/elapsed.Seconds(), 1000*cpu/float64(n), mib(peak))
	}

	if hold > 0 && pid > 0 {
		time.Sleep(time.Second)
		before := rssKB(pid)
		var sessions []string
		for i := 0; i < hold; i++ {
			s, _, err := create()
			if err != nil {
				log.Fatal(err)
			}
			sessions = append(sessions, s)
		}
		time.Sleep(time.Second)
		held := rssKB(pid)
		for _, s := range sessions {
			if err := remove(s); err != nil {
				log.Fatal(err)
			}
		}
		time.Sleep(time.Second)
		after := rssKB(pid)
		fmt.Printf("hold=%d rss_before=%s rss_held=%s rss_after_delete=%s per_copy=%.2fMiB\n",
			hold, mib(before), mib(held), mib(after), float64(held-before)/1024/float64(hold))
	}
	if err := da.DropDatabase(ctx, &databasepb.DropDatabaseRequest{Database: name}); err != nil {
		log.Fatalf("drop database: %v", err)
	}
}

// checkCopy verifies that the session's copy has rows in some table.
func checkCopy(ctx context.Context, sc *spannerapi.Client, session string) error {
	rs, err := sc.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{
		Session: session,
		Sql: `SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES
		  WHERE TABLE_SCHEMA = '' AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME`,
	})
	if err != nil {
		return err
	}
	var tables []string
	for _, r := range rs.Rows {
		tables = append(tables, r.Values[0].GetStringValue())
	}
	sort.Strings(tables)
	total := 0
	for _, t := range tables {
		rs, err := sc.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{
			Session: session, Sql: "SELECT COUNT(*) FROM `" + t + "`",
		})
		if err != nil {
			return err
		}
		var n int
		fmt.Sscan(rs.Rows[0].Values[0].GetStringValue(), &n)
		total += n
	}
	if total == 0 {
		return fmt.Errorf("the copy in %s has no rows", session)
	}
	fmt.Printf("copy check: %d tables, %d rows in the copy\n", len(tables), total)
	return nil
}
