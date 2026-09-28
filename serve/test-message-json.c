/* JSON numbers are read as strtod reads them, bit for bit: in the forms a
 * client writes (a program's shortest digits, fixed decimals, exponents), at
 * the edges of the exact path (2^53, 10^22) and past them, and refused when
 * not finite or not JSON. */
#include "message-json.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *state = x;
}

/* The text read as a number field: true with its value, or false. */
static bool read_number(const char *text, double *value) {
    static const PureField number = {.type = PURE_FIELD_NUMBER};
    unsigned char prefix[8];
    if (!pure_json_fixed(&number, (const unsigned char *)text, strlen(text), prefix)) return false;
    memcpy(value, prefix, sizeof(*value));
    return true;
}

static void same_as_strtod(const char *text) {
    double got, want = strtod(text, NULL);
    CHECK(read_number(text, &got) && !memcmp(&got, &want, sizeof(got)));
}

int main(void) {
    static const char *const edges[] = {
        "0", "-0", "0.0", "-0.0", "1", "0.1", "0.2", "0.30000000000000004", "123456.789", "1e22", "1e23", "1E-22",
        "1e-23", "9007199254740991", "9007199254740992", "9007199254740993", "900719925474099.3", "12345678901234567890",
        "0.000000000000000000000001", "1.7976931348623157e308", "4.9406564584124654e-324", "2.2250738585072014e-308",
        "-123.456e-7", "5e+0", "0.5e1", "1234567890.0987654321"};
    for (size_t i = 0; i < sizeof(edges) / sizeof(*edges); ++i) same_as_strtod(edges[i]);
    static const char *const refused[] = {"1e309", "-1e400", "01", "1.", ".5", "+1", "1e", "--1", "0x10", "1.5e+", "NaN"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); ++i) {
        double value;
        CHECK(!read_number(refused[i], &value));
    }
    uint64_t seed = 0x6a736f6eu;
    size_t read = 0;
    for (unsigned i = 0; i < 400000; ++i) {
        uint64_t bits = next_random(&seed);
        double value;
        switch (i % 4) {
        case 0: value = (double)(int64_t)(bits % 2000001 - 1000000) / 100.0; break;
        case 1: value = (double)(bits % 100000000) / pow(10, (double)(bits >> 50 & 15)); break;
        case 2: memcpy(&value, &bits, sizeof(value)); if (!isfinite(value)) value = -2.5; break;
        default: value = ldexp((double)(bits >> 11), (int)(bits % 120) - 90); break;
        }
        char text[64];
        snprintf(text, sizeof(text), "%.17g", value);
        same_as_strtod(text);
        snprintf(text, sizeof(text), "%.*f", (int)(bits >> 56) % 12, value);
        if (strlen(text) < 40) same_as_strtod(text);
        snprintf(text, sizeof(text), "%.*e", (int)(bits >> 58) % 17, value);
        same_as_strtod(text);
        read += 3;
    }
    printf("{\"json_numbers\":%zu}\n", read);
    return 0;
}
