# Line breaking (UAX #14, Unicode 16.0) in plain Python: the reference 140 counts aup's lines against, the rules in the
# standard's order as Vega's text/linebreak.c has them. A character's properties come from a table of ranges (the
# check builds it from the UCD): its class after LB1, whether it is an initial or a final quotation mark (Pi, Pf), East
# Asian (width F, W or H), and pictographic and unassigned (Extended_Pictographic and Cn).
import bisect

(BK, CR, LF, NL, SP, ZW, ZWJ, CM, WJ, GL, BA, BB, B2, HY, CB, CL, CP, EX, IN, NS, OP, QU, IS, NU, PO, PR, SY, AK, AL, AP,
 AS, EB, EM, H2, H3, HL, ID, JL, JV, JT, RI, VF, VI) = range(43)
NONE, ALLOWED, MANDATORY = 0, 1, 2
ENDS = (BK, CR, LF, NL)


def breaks(codes, starts, rows):
    """Each boundary of codes, the ends included: NONE, ALLOWED or MANDATORY."""
    n = len(codes)
    props = [rows[bisect.bisect_right(starts, c) - 1] for c in codes]  # (class, pi, pf, east, pictographic)
    cls = [p[0] for p in props]
    eff, joined, start, nonspace, number, ri = [0] * n, [False] * n, [0] * n, [None] * n, [False] * n, [0] * n
    for i in range(n):
        mark = cls[i] in (CM, ZWJ)
        previous = eff[i - 1] if i else SP
        joined[i] = mark and i > 0 and previous not in ENDS + (SP, ZW)  # LB9, LB10
        eff[i] = previous if joined[i] else AL if mark else cls[i]
        start[i] = start[i - 1] if joined[i] else i
        nonspace[i] = start[i] if eff[i] != SP else nonspace[i - 1] if i else None
        e = eff[i]
        number[i] = number[i - 1] if joined[i] else e == NU or (e in (SY, IS) and i > 0 and number[i - 1])
        ri[i] = ri[i - 1] if joined[i] else (ri[i - 1] if i else 0) + 1 if e == RI else 0

    def next_unit(i):
        j = i + 1
        while j < n and joined[j]:
            j += 1
        return j

    def aksara(u):
        return eff[u] in (AK, AS) or codes[u] == 0x25CC

    def decide(i):
        a, b, A, B = cls[i - 1], cls[i], eff[i - 1], eff[i]
        unit, before = start[i - 1], nonspace[i - 1]
        base = codes[unit]
        pi, pf, east, pictographic = lambda k: props[k][1], lambda k: props[k][2], lambda k: props[k][3], lambda k: props[k][4]
        if a == BK: return MANDATORY  # LB4
        if a == CR and b == LF: return NONE  # LB5
        if a in (CR, LF, NL): return MANDATORY
        if b in ENDS: return NONE  # LB6
        if b in (SP, ZW): return NONE  # LB7
        if before is not None and eff[before] == ZW: return ALLOWED  # LB8
        if a == ZWJ: return NONE  # LB8a
        if joined[i]: return NONE  # LB9
        if WJ in (A, B): return NONE  # LB11
        if A == GL: return NONE  # LB12
        if B == GL and A not in (SP, BA, HY): return NONE  # LB12a
        if B in (CL, CP, EX, SY): return NONE  # LB13
        if before is not None and eff[before] == OP: return NONE  # LB14
        if before is not None and eff[before] == QU and pi(before):  # LB15a
            if not before or eff[before - 1] in ENDS + (OP, QU, GL, SP, ZW): return NONE
        if B == QU and pf(i):  # LB15b
            j = next_unit(i)
            if j == n or eff[j] in (SP, GL, WJ, CL, QU, CP, EX, IS, SY, BK, CR, LF, NL, ZW): return NONE
        if A == SP and B == IS:  # LB15c
            j = next_unit(i)
            if j < n and eff[j] == NU: return ALLOWED
        if B == IS: return NONE  # LB15d
        if B == NS and before is not None and eff[before] in (CL, CP): return NONE  # LB16
        if B == B2 and before is not None and eff[before] == B2: return NONE  # LB17
        if A == SP: return ALLOWED  # LB18
        if B == QU and not pi(i): return NONE  # LB19
        if A == QU and not pf(unit): return NONE
        if B == QU:  # LB19a
            j = next_unit(i)
            if not east(unit) or j == n or not east(j): return NONE
        if A == QU and (not east(i) or not unit or not east(start[unit - 1])): return NONE
        if CB in (A, B): return ALLOWED  # LB20
        if (A == HY or base == 0x2010) and B == AL:  # LB20a
            if (eff[unit - 1] if unit else SP) in ENDS + (SP, ZW, CB, GL): return NONE
        if B in (BA, HY, NS) or A == BB: return NONE  # LB21
        if (A == HY or (A == BA and not east(unit))) and unit and eff[unit - 1] == HL and B != HL: return NONE  # LB21a
        if A == SY and B == HL: return NONE  # LB21b
        if B == IN: return NONE  # LB22
        if (A in (AL, HL) and B == NU) or (A == NU and B in (AL, HL)): return NONE  # LB23
        if (A == PR and B in (ID, EB, EM)) or (A in (ID, EB, EM) and B == PO): return NONE  # LB23a
        if (A in (PR, PO) and B in (AL, HL)) or (A in (AL, HL) and B in (PR, PO)): return NONE  # LB24
        if B in (PO, PR) and (number[i - 1] or (A in (CL, CP) and unit and number[unit - 1])): return NONE  # LB25
        if A in (PO, PR):
            if B == NU: return NONE
            if B == OP:
                j = next_unit(i)
                if j < n and eff[j] == IS: j = next_unit(j)
                if j < n and eff[j] == NU: return NONE
        if (A in (HY, IS) or number[i - 1]) and B == NU: return NONE
        if A == JL and B in (JL, JV, H2, H3): return NONE  # LB26
        if (A in (JV, H2) and B in (JV, JT)) or (A in (JT, H3) and B == JT): return NONE
        if (A in (JL, JV, JT, H2, H3) and B == PO) or (A == PR and B in (JL, JV, JT, H2, H3)): return NONE  # LB27
        if A in (AL, HL) and B in (AL, HL): return NONE  # LB28
        if A == AP and aksara(i): return NONE  # LB28a
        if aksara(unit) and B in (VF, VI): return NONE
        if A == VI and unit and aksara(start[unit - 1]) and (B == AK or codes[i] == 0x25CC): return NONE
        if aksara(unit) and aksara(i):
            j = next_unit(i)
            if j < n and eff[j] == VF: return NONE
        if A == IS and B in (AL, HL): return NONE  # LB29
        if A in (AL, HL, NU) and B == OP and not east(i): return NONE  # LB30
        if A == CP and not east(unit) and B in (AL, HL, NU): return NONE
        if A == RI and B == RI and ri[i - 1] % 2: return NONE  # LB30a
        if B == EM and (A == EB or pictographic(unit)): return NONE  # LB30b
        return ALLOWED  # LB31

    return [NONE] + [decide(i) for i in range(1, n)] + [MANDATORY] if n else [NONE]
