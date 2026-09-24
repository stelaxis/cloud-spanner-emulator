package main

import (
	"context"
	"encoding/hex"
	"fmt"
	"time"

	spannerpb "cloud.google.com/go/spanner/apiv1/spannerpb"
	"google.golang.org/grpc"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/known/structpb"
	"google.golang.org/protobuf/types/known/timestamppb"
)

// This additive table makes even a lost-ack commit's physical timestamp
// observable after recovery. Its insert is in the SAME commit as A/B writes.
const restartAuditDDL = "CREATE TABLE RestartAudit (K STRING(MAX) NOT NULL, Ts TIMESTAMP OPTIONS (allow_commit_timestamp=true)) PRIMARY KEY (K)"

type restartClient struct {
	spannerpb.SpannerClient
	observe func(*timestamppb.Timestamp)
	clear   bool
}

func (c *restartClient) Commit(ctx context.Context, req *spannerpb.CommitRequest, opts ...grpc.CallOption) (*spannerpb.CommitResponse, error) {
	req = proto.Clone(req).(*spannerpb.CommitRequest)
	if c.clear {
		req.Mutations = append(req.Mutations, &spannerpb.Mutation{Operation: &spannerpb.Mutation_Delete_{Delete: &spannerpb.Mutation_Delete{Table: "RestartAudit", KeySet: &spannerpb.KeySet{All: true}}}})
		c.clear = false
	}
	req.Mutations = append(req.Mutations, &spannerpb.Mutation{Operation: &spannerpb.Mutation_InsertOrUpdate{InsertOrUpdate: &spannerpb.Mutation_Write{
		Table: "RestartAudit", Columns: []string{"K", "Ts"}, Values: []*structpb.ListValue{{Values: []*structpb.Value{
			structpb.NewStringValue(hex.EncodeToString(req.GetTransactionId())), structpb.NewStringValue("spanner.commit_timestamp()"),
		}}},
	}}})
	return c.SpannerClient.Commit(ctx, req, opts...)
}

func (c *restartClient) BeginTransaction(ctx context.Context, req *spannerpb.BeginTransactionRequest, opts ...grpc.CallOption) (*spannerpb.Transaction, error) {
	req = proto.Clone(req).(*spannerpb.BeginTransactionRequest)
	if ro := req.Options.GetReadOnly(); ro != nil {
		ro.ReturnReadTimestamp = true
	}
	tx, err := c.SpannerClient.BeginTransaction(ctx, req, opts...)
	if err == nil {
		c.observe(tx.ReadTimestamp)
	}
	return tx, err
}

func (e *emulator) readRestartAudit(ctx context.Context, observe func(*timestamppb.Timestamp)) error {
	cctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	session, err := e.client.CreateSession(cctx, &spannerpb.CreateSessionRequest{Database: e.database})
	if err != nil {
		return err
	}
	defer func() { _, _ = e.client.DeleteSession(cctx, &spannerpb.DeleteSessionRequest{Name: session.Name}) }()
	rs, err := e.client.Read(cctx, &spannerpb.ReadRequest{Session: session.Name, Table: "RestartAudit", Columns: []string{"K", "Ts"}, KeySet: &spannerpb.KeySet{All: true}, Transaction: &spannerpb.TransactionSelector{Selector: &spannerpb.TransactionSelector_SingleUse{SingleUse: &spannerpb.TransactionOptions{Mode: &spannerpb.TransactionOptions_ReadOnly_{ReadOnly: &spannerpb.TransactionOptions_ReadOnly{TimestampBound: &spannerpb.TransactionOptions_ReadOnly_Strong{Strong: true}}}}}}})
	if err != nil {
		return fmt.Errorf("recovered commit timestamps: %w", err)
	}
	for _, row := range rs.Rows {
		if len(row.Values) != 2 {
			return fmt.Errorf("invalid audit row: %v", row)
		}
		ts, err := time.Parse(time.RFC3339Nano, row.Values[1].GetStringValue())
		if err != nil {
			return fmt.Errorf("recovered commit timestamp: %w", err)
		}
		observe(timestamppb.New(ts))
	}
	return nil
}
