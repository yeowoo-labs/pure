# RFC 6298's retransmission timer in plain Python: the reference 140 counts aup's lines against. A sender's segments
# are sent and acknowledged (cumulatively) at given times; the round-trip time of each acknowledged segment that was
# never sent again (Karn) updates SRTT, RTTVAR and RTO (2.2-2.5: 1 s at least, 60 s at most), and the timer runs as
# 5.1-5.6 say: started when one is sent and none runs, stopped when all are acknowledged, restarted when new data is,
# and on expiry the earliest is sent again and RTO doubles. The timer is seen at ticks of period P, as an app sees time.
LOWEST, HIGHEST = 1000, 60000  # milliseconds; the clock's granularity G is the tick's period


class Sender:
    def __init__(self, period):
        self.period, self.srtt, self.rttvar, self.rto, self.measured = period, 0.0, 0.0, 1000.0, False
        self.deadline, self.outstanding, self.retransmits = -1.0, [], []  # outstanding: [seq, sent, again]

    def send(self, now, seq):
        self.outstanding.append([seq, now, False])
        if self.deadline < 0:
            self.deadline = now + self.rto  # 5.1

    def ack(self, now, seq):
        acked = [s for s in self.outstanding if s[0] <= seq]
        if not acked:
            return
        self.outstanding = [s for s in self.outstanding if s[0] > seq]
        last = acked[-1]
        if not last[2]:  # Karn: no sample from a segment sent again
            r = now - last[1]
            if not self.measured:
                self.srtt, self.rttvar, self.measured = r, r / 2, True  # 2.2
            else:
                self.rttvar = 0.75 * self.rttvar + 0.25 * abs(self.srtt - r)  # 2.3
                self.srtt = 0.875 * self.srtt + 0.125 * r
            self.rto = min(max(self.srtt + max(self.period, 4 * self.rttvar), LOWEST), HIGHEST)  # 2.4, 2.5
        self.deadline = now + self.rto if self.outstanding else -1.0  # 5.3, 5.2

    def tick(self, now):
        if self.deadline >= 0 and now >= self.deadline:  # 5.4-5.6
            self.outstanding[0][2] = True
            self.retransmits.append((self.outstanding[0][0], now))
            self.rto = min(self.rto * 2, HIGHEST)
            self.deadline = now + self.rto
