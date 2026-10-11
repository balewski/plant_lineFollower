"""Offline command accounting. Never treats coalescing as transmission loss."""
import argparse
import csv
import json
from pathlib import Path


def command_report(pi_dir, desktop_dir):
    pi_dir, desktop_dir = Path(pi_dir), Path(desktop_dir)
    ps = json.loads((pi_dir / 'summary.json').read_text())
    ds = json.loads((desktop_dir / 'summary.json').read_text())
    with (pi_dir / 'events.csv').open() as f:
        pr = list(csv.DictReader(f))
    with (desktop_dir / 'events.csv').open() as f:
        dr = list(csv.DictReader(f))
    pc, dc = ps['counters'], ds['counters']
    issued = {int(r['b']) for r in dr if r['event'] == 'decision'}
    accepted = {int(r['a']) for r in pr if r['event'] == 'command'}
    dispatched = {int(r['a']) for r in pr if r['event'] == 'uart_tx'
                  and r['detail'].startswith('MOTORST ')}
    acknowledged = {int(r['a']) for r in pr if r['event'] == 'teensy_target_rx'}
    replaced = pc.get('pending_commands_replaced', 0)
    checks = [
        (not pc.get('audit_overwritten', 0) and not dc.get('audit_overwritten', 0), 'audit log truncated'),
        (len(issued) == dc['commands_sent'], 'issued counter/log mismatch'),
        (len(accepted) == pc['commands_accepted'], 'Pi received counter/log mismatch'),
        (len(acknowledged) == pc['command_acks'], 'ACK counter/log mismatch'),
        (acknowledged <= dispatched <= accepted <= issued, 'command IDs inconsistent between logs'),
        (0 <= replaced <= len(accepted - dispatched), 'replacement count inconsistent'),
    ]
    problems = [reason for ok, reason in checks if not ok]
    valid = not problems
    missing_at_pi = len(issued - accepted)
    unconfirmed_uart = len(dispatched - acknowledged)
    other_unsent = len(accepted - dispatched) - replaced
    unresolved = missing_at_pi + unconfirmed_uart + other_unsent if valid else None
    result = dict(schema_version=1, valid=valid, problems=problems,
                  issued=len(issued), pi_received=len(accepted),
                  uart_dispatched=len(dispatched), teensy_received_confirmed=len(acknowledged),
                  replaced_before_uart=replaced, not_accepted_by_pi=missing_at_pi,
                  uart_without_ack=unconfirmed_uart, other_unsent=other_unsent,
                  lost_or_unconfirmed=unresolved,
                  confirmed_lost=0 if unresolved == 0 else None,
                  note='Received at Teensy is confirmed by its UART ACK. Missing ACKs do not prove command loss. Replaced commands are excluded from loss. Counts cover the full job, even in a trimmed video.')
    if valid:
        result['lines'] = [f'RUN CMD issued {len(issued)} | Pi RX {len(accepted)} | Teensy RX {len(acknowledged)}',
                           f'Replaced @ Pi {replaced} | Lost/unconfirmed {unresolved}']
    else:
        result['lines'] = ['CMD counts unavailable: incomplete logs', 'See command_report.json for details']
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pi', required=True)
    ap.add_argument('--desktop', required=True)
    ap.add_argument('--output', required=True)
    args = ap.parse_args()
    report = command_report(args.pi, args.desktop)
    Path(args.output).write_text(json.dumps(report, indent=2) + '\n')
    print('\n'.join(report['lines']))
