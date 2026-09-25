package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
)

// model drives `txnmodel --batch`: one schedule per line in, one result array
// per line out.
type model struct {
	cmd *exec.Cmd
	in  io.WriteCloser
	out *bufio.Reader
}

func startModel(path, name string, pushdown bool, persistence ...string) (*model, error) {
	args := []string{"--model", name, "--batch"}
	if pushdown {
		args = append(args, "--pushdown")
	}
	if len(persistence) > 0 {
		args = append(args, "--persistence", persistence[0])
	}
	cmd := exec.Command(path, args...)
	cmd.Stderr = os.Stderr
	in, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start %s: %w", path, err)
	}
	return &model{cmd: cmd, in: in, out: bufio.NewReaderSize(out, 1<<20)}, nil
}

func (m *model) run(steps []Step) ([]Outcome, error) {
	line, err := json.Marshal(steps)
	if err != nil {
		return nil, err
	}
	if _, err := m.in.Write(append(line, '\n')); err != nil {
		return nil, err
	}
	resp, err := m.out.ReadBytes('\n')
	if err != nil {
		return nil, fmt.Errorf("read model output: %w", err)
	}
	var raws []json.RawMessage
	if err := json.Unmarshal(resp, &raws); err != nil {
		return nil, fmt.Errorf("model output %q: %w", resp, err)
	}
	if len(raws) != len(steps) {
		return nil, fmt.Errorf("model returned %d results for %d steps", len(raws), len(steps))
	}
	out := make([]Outcome, len(raws))
	for i, raw := range raws {
		if out[i], err = parseModelResult(raw); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func (m *model) close() {
	_ = m.in.Close()
	_ = m.cmd.Wait()
}
