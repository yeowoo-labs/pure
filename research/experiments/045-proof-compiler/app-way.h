/* Research 045: UAX #14's app (aup's examples/linebreak.aup: on line, decide and the fns it calls), written once by hand
 * as an emitter would write it, against the macros a way (app.c) defines first: how numbers that are whole are held
 * (IDX) and computed, how an index is checked, how the event's lists, the message's points and the stored ranges are
 * held and read (LIST_N numbers, LIST_I whole numbers, LIST_B bools), what a record of lists takes (CAPTURE), and what
 * the budget charges. Included once a way, with WAY
 * the suffix of its names. Locals are C's (registers) and each aup fn a C function in every way.
 *
 * C's && and || go no further than they must; where the executor evaluates both sides, this does less (so the first
 * way is cheaper than the module's code would be, never dearer). */
#define JOIN2(a, b) a##_##b
#define JOIN(a, b) JOIN2(a, b)
#define FN(name) JOIN(name, WAY)

/* -1 aside, every value the app keeps of a code point's position: the result of a call that faulted is 0 and x->fault set. */
#define FAIL do { x->fault = true; x->fault_line = __LINE__; return 0; } while (0)
#define CALLED(e) ({ __typeof__(e) called_ = (e); if (x->fault) return 0; called_; })

typedef struct { LIST_N codes, cls, eff; LIST_I start, nonspace, ri, at; LIST_B joined, numeric; } FN(Text);

static double FN(range)(CONTEXT *x, double c) {
    CHARGE(CALL_COST);
#ifdef RANGE_SEEK /* the halving done by the tree's own order (a way that says why it may) */
    return (double)RANGE_SEEK(c);
#endif
    IDX low = 0, high = R_COUNT;
    while (GT(SUB(high, low), 1)) {
        CHARGE(ROUND_COST);
        IDX middle = DIV2(ADD(low, high));
        if (R_START(middle) <= c) low = middle;
        else high = middle;
    }
    return (double)low;
}

static IDX FN(next_unit)(CONTEXT *x, const FN(Text) *t, IDX i) {
    CHARGE(CALL_COST);
    IDX j = ADD(i, 1);
    while (j < NCOUNT(t->codes) && GETB(t->joined, j)) {
        CHARGE(ROUND_COST);
        j = ADD(j, 1);
    }
    return j;
}

static bool FN(aksara)(CONTEXT *x, const FN(Text) *t, IDX u) {
    CHARGE(CALL_COST);
    return GETN(t->eff, u) == AK || GETN(t->eff, u) == AS || GETN(t->codes, u) == 0x25CC;
}
static bool FN(pi)(CONTEXT *x, const FN(Text) *t, IDX k) { CHARGE(CALL_COST); return R_PI(GETI(t->at, k)); }
static bool FN(pf)(CONTEXT *x, const FN(Text) *t, IDX k) { CHARGE(CALL_COST); return R_PF(GETI(t->at, k)); }
static bool FN(east)(CONTEXT *x, const FN(Text) *t, IDX k) { CHARGE(CALL_COST); return R_EAST(GETI(t->at, k)); }

#define END(c) ((c) == BK || (c) == CR || (c) == LF || (c) == NL)
#define NEXT_UNIT(i) CALLED(FN(next_unit)(x, t, (i)))
#define AKSARA(u) CALLED(FN(aksara)(x, t, (u)))
#define PI(k) CALLED(FN(pi)(x, t, (k)))
#define PF(k) CALLED(FN(pf)(x, t, (k)))
#define EAST(k) CALLED(FN(east)(x, t, (k)))

static int FN(decide)(CONTEXT *x, const FN(Text) *t, IDX i) {
    CHARGE(CALL_COST);
    IDX n = NCOUNT(t->codes);
    double a = GETN(t->cls, SUB(i, 1)), b = GETN(t->cls, i), A = GETN(t->eff, SUB(i, 1)), B = GETN(t->eff, i);
    IDX unit = GETI(t->start, SUB(i, 1)), before = GETI(t->nonspace, SUB(i, 1));
    double base = GETN(t->codes, unit);
    if (a == BK) return MANDATORY;                                                          /* LB4 */
    if (a == CR && b == LF) return NONE;                                                    /* LB5 */
    if (a == CR || a == LF || a == NL) return MANDATORY;
    if (END(b)) return NONE;                                                                /* LB6 */
    if (b == SP || b == ZW) return NONE;                                                    /* LB7 */
    if (before >= 0 && GETN(t->eff, before) == ZW) return ALLOWED;                          /* LB8 */
    if (a == ZWJ) return NONE;                                                              /* LB8a */
    if (GETB(t->joined, i)) return NONE;                                                    /* LB9 */
    if (A == WJ || B == WJ) return NONE;                                                    /* LB11 */
    if (A == GL) return NONE;                                                               /* LB12 */
    if (B == GL && A != SP && A != BA && A != HY) return NONE;                              /* LB12a */
    if (B == CL || B == CP || B == EX || B == SY) return NONE;                              /* LB13 */
    if (before >= 0 && GETN(t->eff, before) == OP) return NONE;                             /* LB14 */
    if (before >= 0 && GETN(t->eff, before) == QU && PI(before)) {                          /* LB15a */
        if (before == 0) return NONE;
        double q = GETN(t->eff, SUB(before, 1));
        if (END(q) || q == OP || q == QU || q == GL || q == SP || q == ZW) return NONE;
    }
    if (B == QU && PF(i)) {                                                                 /* LB15b */
        IDX j = NEXT_UNIT(i);
        if (j == n) return NONE;
        double e = GETN(t->eff, j);
        if (e == SP || e == GL || e == WJ || e == CL || e == QU || e == CP || e == EX || e == IS || e == SY || END(e) || e == ZW)
            return NONE;
    }
    if (A == SP && B == IS) {                                                               /* LB15c */
        IDX j = NEXT_UNIT(i);
        if (j < n && GETN(t->eff, j) == NU) return ALLOWED;
    }
    if (B == IS) return NONE;                                                               /* LB15d */
    if (B == NS && before >= 0 && (GETN(t->eff, before) == CL || GETN(t->eff, before) == CP)) return NONE; /* LB16 */
    if (B == B2 && before >= 0 && GETN(t->eff, before) == B2) return NONE;                  /* LB17 */
#ifdef BREAK_ONE_RULE
    if (A == SP) return NONE; /* a rule made wrong: the check must refuse the way */
#else
    if (A == SP) return ALLOWED;                                                            /* LB18 */
#endif
    if (B == QU && !PI(i)) return NONE;                                                     /* LB19 */
    if (A == QU && !PF(unit)) return NONE;
    if (B == QU) {                                                                          /* LB19a */
        IDX j = NEXT_UNIT(i);
        if (!EAST(unit) || j == n || !EAST(j)) return NONE;
    }
    if (A == QU && (!EAST(i) || unit == 0 || !EAST(GETI(t->start, SUB(unit, 1))))) return NONE;
    if (B == CB || A == CB) return ALLOWED;                                                 /* LB20 */
    if ((A == HY || base == 0x2010) && B == AL) {                                           /* LB20a */
        double w = unit > 0 ? GETN(t->eff, SUB(unit, 1)) : SP;
        if (END(w) || w == SP || w == ZW || w == CB || w == GL) return NONE;
    }
    if (B == BA || B == HY || B == NS || A == BB) return NONE;                              /* LB21 */
    if ((A == HY || (A == BA && !EAST(unit))) && unit > 0 && GETN(t->eff, SUB(unit, 1)) == HL && B != HL) return NONE; /* LB21a */
    if (A == SY && B == HL) return NONE;                                                    /* LB21b */
    if (B == IN) return NONE;                                                               /* LB22 */
    if (((A == AL || A == HL) && B == NU) || (A == NU && (B == AL || B == HL))) return NONE; /* LB23 */
    if ((A == PR && (B == ID || B == EB || B == EM)) || ((A == ID || A == EB || A == EM) && B == PO)) return NONE; /* LB23a */
    if (((A == PR || A == PO) && (B == AL || B == HL)) || ((A == AL || A == HL) && (B == PR || B == PO))) return NONE; /* LB24 */
    if ((B == PO || B == PR) &&
        (GETB(t->numeric, SUB(i, 1)) || ((A == CL || A == CP) && unit > 0 && GETB(t->numeric, SUB(unit, 1)))))
        return NONE;                                                                        /* LB25 */
    if (A == PO || A == PR) {
        if (B == NU) return NONE;
        if (B == OP) {
            IDX j = NEXT_UNIT(i);
            if (j < n && GETN(t->eff, j) == IS) j = NEXT_UNIT(j);
            if (j < n && GETN(t->eff, j) == NU) return NONE;
        }
    }
    if ((A == HY || A == IS || GETB(t->numeric, SUB(i, 1))) && B == NU) return NONE;
    if (A == JL && (B == JL || B == JV || B == H2 || B == H3)) return NONE;                 /* LB26 */
    if (((A == JV || A == H2) && (B == JV || B == JT)) || ((A == JT || A == H3) && B == JT)) return NONE;
    bool jamo = A == JL || A == JV || A == JT || A == H2 || A == H3;                         /* LB27 */
    if ((jamo && B == PO) || (A == PR && (B == JL || B == JV || B == JT || B == H2 || B == H3))) return NONE;
    if ((A == AL || A == HL) && (B == AL || B == HL)) return NONE;                          /* LB28 */
    if (A == AP && AKSARA(i)) return NONE;                                                  /* LB28a */
    if (AKSARA(unit) && (B == VF || B == VI)) return NONE;
    if (A == VI && unit > 0 && AKSARA(GETI(t->start, SUB(unit, 1))) && (B == AK || GETN(t->codes, i) == 0x25CC)) return NONE;
    if (AKSARA(unit) && AKSARA(i)) {
        IDX j = NEXT_UNIT(i);
        if (j < n && GETN(t->eff, j) == VF) return NONE;
    }
    if (A == IS && (B == AL || B == HL)) return NONE;                                       /* LB29 */
    if ((A == AL || A == HL || A == NU) && B == OP && !EAST(i)) return NONE;                /* LB30 */
    if (A == CP && !EAST(unit) && (B == AL || B == HL || B == NU)) return NONE;
    if (A == RI && B == RI && MOD2(GETI(t->ri, SUB(i, 1))) == 1) return NONE;              /* LB30a */
    if (B == EM && (A == EB || R_PICT(GETI(t->at, unit)))) return NONE;                     /* LB30b */
    return ALLOWED;                                                                         /* LB31 */
}

/* on line: the boundaries of the line's points into got (0 none, 1 allowed, 2 mandatory), count + 1 of them; false on a fault. */
static bool FN(on_line)(CONTEXT *x, const POINTS *points, uint8_t *got) {
    LINE_BEGIN;
    for (IDX k = 0; k < P_COUNT(points); k = k + 1) {
        CHARGE(ROUND_COST);
        double code = P_CODE(points, k);
        IDX i = NCOUNT(L(codes));
        IDX r = (IDX)CALLED(FN(range)(x, code));
        double c = R_CLASS(r);
        bool mark = c == CM || c == ZWJ;
        double previous = i > 0 ? GETN(L(eff), SUB(i, 1)) : SP;
        bool joins = mark && i > 0 && !END(previous) && previous != SP && previous != ZW; /* LB9, LB10 */
        double e = joins ? previous : mark ? AL : c;
        APPN(L(codes), code);
        APPN(L(cls), c);
        APPI(L(at), r);
        APPB(L(joined), joins);
        APPN(L(eff), e);
        APPI(L(start), joins ? GETI(L(start), SUB(i, 1)) : i);
        APPI(L(nonspace), e != SP ? GETI(L(start), i) : i > 0 ? GETI(L(nonspace), SUB(i, 1)) : (IDX)-1);
        APPB(L(numeric), joins ? GETB(L(numeric), SUB(i, 1)) : e == NU || ((e == SY || e == IS) && i > 0 && GETB(L(numeric), SUB(i, 1))));
        APPI(L(ri), joins ? GETI(L(ri), SUB(i, 1)) : e == RI ? ADD(i > 0 ? GETI(L(ri), SUB(i, 1)) : (IDX)0, 1) : (IDX)0);
    }
    FN(Text) t;
    CAPTURE(t.codes, codes); CAPTURE(t.cls, cls); CAPTURE(t.eff, eff); CAPTURE(t.joined, joined); CAPTURE(t.start, start);
    CAPTURE(t.nonspace, nonspace); CAPTURE(t.numeric, numeric); CAPTURE(t.ri, ri); CAPTURE(t.at, at);
    OUT_APPEND(0);                                                                          /* LB2 */
    for (IDX i = 1; i < NCOUNT(L(codes)); i = i + 1) {
        CHARGE(ROUND_COST);
        OUT_APPEND(CALLED(FN(decide)(x, &t, i)));
    }
    if (NCOUNT(L(codes)) > 0) OUT_APPEND(2);                                                /* LB3 */
    OUT_EMIT(got);
    LINE_END;
    return true;
}

#undef END
#undef NEXT_UNIT
#undef AKSARA
#undef PI
#undef PF
#undef EAST
