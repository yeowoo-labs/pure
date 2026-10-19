#!/usr/bin/env python3
"""Research 140: RFC 6298's retransmission timer written in aup (aup's examples/rto.aup), with periodic ticks.

Each scenario (a seed and a tick period P) is made closed-loop by rto.py's model and a receiver: segments sent at random
gaps, each arriving half a random round trip later unless its first sending is lost (one in five), the receiver
acknowledging cumulatively all it holds without a gap, the acknowledgement arriving after the other half; a segment the
timer sends again arrives. The events (sends, acknowledgements, ticks every P ms) are then given to the app
through pure-run (time MS before each), on Pure's three paths: after every event its SRTT, RTTVAR, RTO, deadline, the
segments outstanding and the retransmissions must be the model's, the paths the same. No known implementation is
compared: the oracle is the model of the same rules. What the ticks cost is measured: ticks a timeout (a one-shot timer
would be one event), and how late a timeout is seen (below P). Then counts lines of rto.aup and of rto.py's model. One
JSON line."""
import json, os, random, struct, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
pure = os.path.normpath(os.path.join(here, '../../..'))
aup = os.environ.get('AUP', os.path.join(pure, '..', 'aup'))
sys.path.insert(0, here)
import rto  # noqa: E402

START = 1_000_000  # the first event's time (ms since 1970, as now is)


def lines(path, stop=None):
    code = braces = 0
    for line in open(path):
        if stop and line.startswith(stop): break
        text = line.strip()
        if not text or text.startswith('#'): continue
        code += 1
        braces += text == '}'
    return code, braces


def scenario(seed, period, segments=40):
    """The events the app is given (time, kind, seq), the model's state after each, and the timeouts' lateness. The
    network: a sending arrives half a random round trip later unless it is lost (a first sending, one in five), and the
    receiver acknowledges at once, cumulatively, all it holds without a gap; the acknowledgement arrives after the other
    half."""
    chooser = random.Random(seed)
    sender = rto.Sender(period)
    pending, t = [], START + period  # (time, order, kind, seq): arrivals, then acknowledgements, sends, then ticks
    for seq in range(1, segments + 1):
        t += chooser.choice([0, 0, 5, 40, 120, 300, 900])
        pending.append((t, 2, 'transmit', seq))
    held, through = set(), 0  # the receiver's segments, and the last one it holds every one up to

    def sent(time, seq, first):
        if not first or chooser.random() > 0.2:
            half = chooser.randrange(20, 350)
            pending.append((time + half, 0, 'arrive', seq * 1000 + half))

    events, states, late = [], [], []
    tick = START
    while pending or sender.outstanding:
        pending.sort()
        if pending and pending[0][0] < tick + period:  # the host ticks every P, the timer running or not
            time, _, kind, seq = pending.pop(0)
        else:
            time, kind, seq = tick + period, 'tick', 0
            tick = time
        if kind == 'arrive':  # the receiver's: not the app's
            seq, half = divmod(seq, 1000)
            held.add(seq)
            while through + 1 in held:
                through += 1
            if through:
                pending.append((time + half, 1, 'ack', through))
            continue
        if kind == 'transmit':
            sender.send(time, seq)
            sent(time, seq, True)
        elif kind == 'ack':
            sender.ack(time, seq)
        else:
            before, deadline = len(sender.retransmits), sender.deadline
            sender.tick(time)
            if len(sender.retransmits) > before:
                late.append(time - deadline)
                sent(time, sender.retransmits[-1][0], False)
        events.append((time, kind, seq))
        states.append((sender.srtt, sender.rttvar, sender.rto, sender.deadline, len(sender.outstanding), len(sender.retransmits)))
        if len(events) > 200000:
            raise RuntimeError('a scenario without end')
    return events, states, late


def run(module, events, env, work):
    commands = os.path.join(work, 'rto.commands')
    with open(commands, 'w') as f:
        for time, kind, seq in events:
            f.write(f'time {time}\n')
            f.write('tick tick\n' if kind == 'tick' else f"message {kind} {struct.pack('<I', seq).hex()}\n")
    got = []
    with open(commands) as stdin:
        process = subprocess.Popen([os.path.join(pure, 'build', 'pure-run'), module], stdin=stdin, stdout=subprocess.PIPE,
                                   text=True, env=dict(os.environ, **env))
        for k, line in enumerate(process.stdout):
            if k % 2 == 0: continue  # a time command's
            state = json.loads(line)
            s = state['states']
            got.append((state['fault'], s['srtt'], s['rttvar'], s['rto'], s['deadline'], len(s['outstanding']),
                        len(s['retransmits'])))
        process.wait()
    return got


if __name__ == '__main__':
    report = {'scenarios': 0, 'events': 0, 'timeouts': 0, 'right': {}, 'paths_same': True}
    costs = {}
    seeds = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    with tempfile.TemporaryDirectory() as work:
        source = open(os.path.join(aup, 'examples', 'rto.aup')).read()
        for period in (10, 100):
            module = os.path.join(work, f'rto-{period}.pmod')  # its tick the scenario's period
            with open(os.path.join(work, f'rto-{period}.aup'), 'w') as f:
                f.write(source.replace('every 10 ms', f'every {period} ms'))  # G, tick.period, follows
            subprocess.run([os.path.join(aup, 'build', 'aup'), 'build', os.path.join(work, f'rto-{period}.aup'), '-o', module], check=True)
            ticks = timeouts = 0
            lateness = []
            for seed in range(seeds):
                events, states, late = scenario(seed, period)
                outputs = {}
                for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}),
                                  ('all', {'PURE_JIT': 'all'})):
                    got = run(module, events, env, work)
                    want = [('none',) + s for s in states]
                    report['right'][name] = report['right'].get(name, 0) + sum(g == w for g, w in zip(got, want))
                    outputs[name] = got
                report['paths_same'] = report['paths_same'] and outputs['rounds'] == outputs['batches'] == outputs['all']
                report['scenarios'] += 1
                report['events'] += len(events)
                ticks += sum(kind == 'tick' for _, kind, _ in events)
                timeouts += len(late)
                lateness += late
            report['timeouts'] += timeouts
            costs[period] = {'ticks': ticks, 'timeouts': timeouts, 'ticks_a_timeout': round(ticks / max(timeouts, 1), 1),
                             'late_mean_ms': round(sum(lateness) / max(len(lateness), 1), 2), 'late_most_ms': max(lateness, default=0)}
    report['costs'] = costs
    aup_code, aup_braces = lines(os.path.join(aup, 'examples', 'rto.aup'))
    python_code, _ = lines(os.path.join(here, 'rto.py'))
    report.update({'aup_lines': aup_code, 'aup_brace_lines': aup_braces, 'python_lines': python_code})
    ok = all(n == report['events'] for n in report['right'].values()) and report['paths_same']
    print(json.dumps(report))
    sys.exit(0 if ok else 1)
