package main

// Ephemeral sessions (docs/ephemeral-sessions.md) against their design: an
// ephemeral session's copy is the base database's state when the session was
// created, and afterwards changes only by the session's own writes.
//
// Each run starts from empty copyTables and performs random steps from one
// goroutine: commits to the base, creating an ephemeral session, commits to a
// copy (mutations or DML), full reads of the base or a copy, and deleting an
// ephemeral session. The model keeps one table state per database; every
// read's rows must equal the model's.

import (
	"context"
	"fmt"
	"maps"
	"math/rand/v2"
	"slices"
	"strconv"

	spannerpb "cloud.google.com/go/spanner/apiv1/spannerpb"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/structpb"
)

// copyTables: table -> key -> value.
type copyTables map[string]map[int64]int64

func (t copyTables) clone() copyTables {
	c := copyTables{}
	for name, rows := range t {
		c[name] = maps.Clone(rows)
	}
	return c
}

type ephemeralStats struct {
	runs, steps, copies, copyWrites, reads, deletes int
}

// runEphemeral returns an error on the first mismatch.
func runEphemeral(ctx context.Context, e *emulator, runs int, firstSeed uint64, numKeys int) (ephemeralStats, error) {
	var st ephemeralStats
	for i := range runs {
		seed := firstSeed + uint64(i)
		if err := ephemeralRun(ctx, e, rand.New(rand.NewPCG(seed, 0xe9)), numKeys, &st); err != nil {
			return st, fmt.Errorf("seed %d: %w", seed, err)
		}
		st.runs++
	}
	return st, nil
}

func ephemeralRun(ctx context.Context, e *emulator, r *rand.Rand, numKeys int, st *ephemeralStats) error {
	base, err := e.createSession(ctx, nil)
	if err != nil {
		return err
	}
	defer e.deleteSession(ctx, base)
	// Clear the base.
	var clear []*spannerpb.Mutation
	for _, t := range []string{"A", "B"} {
		clear = append(clear, &spannerpb.Mutation{Operation: &spannerpb.Mutation_Delete_{
			Delete: &spannerpb.Mutation_Delete{Table: t, KeySet: &spannerpb.KeySet{All: true}}}})
	}
	if err := e.commitMutations(ctx, base, clear); err != nil {
		return err
	}
	model := map[string]copyTables{base: {"A": {}, "B": {}}}
	var copies []string
	var deleted []string
	defer func() {
		for _, c := range copies {
			e.deleteSession(ctx, c)
		}
	}()

	randomMutations := func(t copyTables) []*spannerpb.Mutation {
		var ms []*spannerpb.Mutation
		for range 1 + r.IntN(4) {
			table := []string{"A", "B"}[r.IntN(2)]
			k := int64(r.IntN(numKeys))
			if r.IntN(3) == 0 {
				ms = append(ms, &spannerpb.Mutation{Operation: &spannerpb.Mutation_Delete_{
					Delete: &spannerpb.Mutation_Delete{Table: table, KeySet: &spannerpb.KeySet{
						Keys: []*structpb.ListValue{{Values: []*structpb.Value{intValue(k)}}}}}}})
				delete(t[table], k)
				continue
			}
			v := int64(r.IntN(100))
			ms = append(ms, &spannerpb.Mutation{Operation: &spannerpb.Mutation_InsertOrUpdate{
				InsertOrUpdate: &spannerpb.Mutation_Write{Table: table, Columns: []string{"K", "V"},
					Values: []*structpb.ListValue{{Values: []*structpb.Value{intValue(k), intValue(v)}}}}}})
			t[table][k] = v
		}
		return ms
	}
	check := func(session string) error {
		st.reads++
		got, err := e.readAll(ctx, session)
		if err != nil {
			return err
		}
		want := model[session]
		for _, t := range []string{"A", "B"} {
			if !maps.Equal(got[t], want[t]) {
				return fmt.Errorf("session %s table %s: emulator %v, model %v", session, t, got[t], want[t])
			}
		}
		return nil
	}

	steps := 10 + r.IntN(20)
	for range steps {
		st.steps++
		switch op := r.IntN(10); {
		case op < 3: // base commit
			if err := e.commitMutations(ctx, base, randomMutations(model[base])); err != nil {
				return err
			}
		case op < 5 && len(copies) < 4: // new ephemeral session
			c, err := e.createSession(ctx, map[string]string{"emulator-ephemeral": "true"})
			if err != nil {
				return err
			}
			st.copies++
			copies = append(copies, c)
			model[c] = model[base].clone()
		case op < 7 && len(copies) > 0: // copy commit
			c := copies[r.IntN(len(copies))]
			st.copyWrites++
			if r.IntN(2) == 0 {
				if err := e.commitMutations(ctx, c, randomMutations(model[c])); err != nil {
					return err
				}
			} else if err := e.dmlOnCopy(ctx, c, r, numKeys, model[c]); err != nil {
				return err
			}
		case op < 9: // read
			all := append([]string{base}, copies...)
			if err := check(all[r.IntN(len(all))]); err != nil {
				return err
			}
		case len(copies) > 0: // delete an ephemeral session
			i := r.IntN(len(copies))
			st.deletes++
			if err := e.deleteSession(ctx, copies[i]); err != nil {
				return err
			}
			deleted = append(deleted, copies[i])
			delete(model, copies[i])
			copies = slices.Delete(copies, i, i+1)
		}
	}
	for _, s := range append([]string{base}, copies...) {
		if err := check(s); err != nil {
			return err
		}
	}
	for _, s := range deleted {
		if _, err := e.readAll(ctx, s); status.Code(err) != codes.NotFound {
			return fmt.Errorf("deleted session %s: got %v, want NOT_FOUND", s, err)
		}
	}
	return nil
}

func (e *emulator) createSession(ctx context.Context, labels map[string]string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	s, err := e.client.CreateSession(ctx, &spannerpb.CreateSessionRequest{
		Database: e.database, Session: &spannerpb.Session{Labels: labels}})
	if err != nil {
		return "", fmt.Errorf("create session: %w", err)
	}
	return s.Name, nil
}

func (e *emulator) deleteSession(ctx context.Context, session string) error {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	_, err := e.client.DeleteSession(ctx, &spannerpb.DeleteSessionRequest{Name: session})
	return err
}

func (e *emulator) commitMutations(ctx context.Context, session string, ms []*spannerpb.Mutation) error {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	_, err := e.client.Commit(ctx, &spannerpb.CommitRequest{
		Session: session, Mutations: ms,
		Transaction: &spannerpb.CommitRequest_SingleUseTransaction{SingleUseTransaction: &spannerpb.TransactionOptions{
			Mode: &spannerpb.TransactionOptions_ReadWrite_{ReadWrite: &spannerpb.TransactionOptions_ReadWrite{}}}},
	})
	if err != nil {
		return fmt.Errorf("commit: %w", err)
	}
	return nil
}

// dmlOnCopy runs one UPDATE or DELETE over a key range in a read-write
// transaction and applies it to `t`.
func (e *emulator) dmlOnCopy(ctx context.Context, session string, r *rand.Rand, numKeys int, t copyTables) error {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	txn, err := e.client.BeginTransaction(ctx, &spannerpb.BeginTransactionRequest{Session: session,
		Options: &spannerpb.TransactionOptions{Mode: &spannerpb.TransactionOptions_ReadWrite_{
			ReadWrite: &spannerpb.TransactionOptions_ReadWrite{}}}})
	if err != nil {
		return fmt.Errorf("begin: %w", err)
	}
	table := []string{"A", "B"}[r.IntN(2)]
	lo := int64(r.IntN(numKeys))
	var sql string
	if r.IntN(2) == 0 {
		v := int64(r.IntN(100))
		sql = fmt.Sprintf("UPDATE %s SET V = %d WHERE K >= %d", table, v, lo)
		for k := range t[table] {
			if k >= lo {
				t[table][k] = v
			}
		}
	} else {
		sql = fmt.Sprintf("DELETE FROM %s WHERE K >= %d", table, lo)
		for k := range t[table] {
			if k >= lo {
				delete(t[table], k)
			}
		}
	}
	if _, err := e.client.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{Session: session, Sql: sql, Seqno: 1,
		Transaction: &spannerpb.TransactionSelector{Selector: &spannerpb.TransactionSelector_Id{Id: txn.Id}}}); err != nil {
		return fmt.Errorf("dml: %w", err)
	}
	if _, err := e.client.Commit(ctx, &spannerpb.CommitRequest{Session: session,
		Transaction: &spannerpb.CommitRequest_TransactionId{TransactionId: txn.Id}}); err != nil {
		return fmt.Errorf("commit dml: %w", err)
	}
	return nil
}

// readAll reads both copyTables with a strong single-use read.
func (e *emulator) readAll(ctx context.Context, session string) (copyTables, error) {
	ctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	out := copyTables{}
	for _, t := range []string{"A", "B"} {
		rs, err := e.client.ExecuteSql(ctx, &spannerpb.ExecuteSqlRequest{Session: session,
			Sql: "SELECT K, V FROM " + t + " ORDER BY K"})
		if err != nil {
			return nil, err
		}
		out[t] = map[int64]int64{}
		for _, row := range rs.Rows {
			k, _ := strconv.ParseInt(row.Values[0].GetStringValue(), 10, 64)
			v, _ := strconv.ParseInt(row.Values[1].GetStringValue(), 10, 64)
			out[t][k] = v
		}
	}
	return out, nil
}
