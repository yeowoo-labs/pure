#include "program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Build-time preparation. The generated initializer is compiled with the
 * target, so the run-only host contains neither this tool nor the parser. */
static void region(FILE *out, PureRegion value) {
    fprintf(out, "{.offset=%zu,.length=%zu,.to_end=%s}", value.offset,
            value.length, value.to_end ? "true" : "false");
}

static void index_value(FILE *out, PureIndex value) {
    fprintf(out, "{.value=%zu,.from_u32le=%s}", value.value,
            value.from_u32le ? "true" : "false");
}

static void emit(FILE *out, const PureProgram *p) {
    fputs("/* Generated from a validated .pure definition. Do not edit. */\n"
          "static const PureProgram pure_embedded_program = {\n.read=", out);
    region(out, p->read);
    fputs(",\n.write=", out);
    region(out, p->write);
    fputs(",\n.reply=", out);
    region(out, p->reply);
    fprintf(out, ",\n.read_required=%zu,.input_required=%zu,\n"
            ".view_definitions=%zu,.range_definitions=%zu,\n"
            ".write_step=%zu,.reply_step=%zu,.input_steps=%zu,\n",
            p->read_required, p->input_required, p->view_definitions,
            p->range_definitions, p->write_step, p->reply_step, p->input_steps);
    if (p->input_steps) fputs(".steps={\n", out);
    for (size_t i = 0; i < p->input_steps; ++i) {
        const PureRangeStep *step = &p->steps[i];
        fputs("{.source=", out);
        region(out, step->source);
        fprintf(out, ",.source_step=%zu,.offset=", step->source_step);
        index_value(out, step->offset);
        fputs(",.length=", out);
        index_value(out, step->length);
        fputs("},\n", out);
    }
    if (p->input_steps) fputs("},\n", out);
    fprintf(out, ".reply_input=%s,.splice=%s,\n.splice_offset=",
            p->reply_input ? "true" : "false", p->splice ? "true" : "false");
    index_value(out, p->splice_offset);
    fputs(",.splice_remove=", out);
    index_value(out, p->splice_remove);
    fprintf(out, ",.splice_limit=%u\n};\n", p->splice_limit);
}

static bool same_file(const char *left_path, const char *right_path) {
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    if (!left || !right) {
        if (left) fclose(left);
        if (right) fclose(right);
        return false;
    }
    bool same = true;
    for (;;) {
        int a = fgetc(left), b = fgetc(right);
        if (a != b) { same = false; break; }
        if (a == EOF) break;
    }
    same = same && !ferror(left) && !ferror(right);
    int left_status = fclose(left), right_status = fclose(right);
    if (left_status || right_status) same = false;
    return same;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: pure-compile SOURCE.pure OUTPUT.h\n", stderr);
        return 2;
    }
    struct stat source_info, output_info;
    if (!stat(argv[1], &source_info) && !stat(argv[2], &output_info) &&
        source_info.st_dev == output_info.st_dev &&
        source_info.st_ino == output_info.st_ino) {
        fputs("source and output must be different files\n", stderr);
        return 2;
    }
    PureProgram program;
    char error[160];
    if (!pure_program_load(argv[1], &program, error, sizeof(error))) {
        fprintf(stderr, "Program: %s\n", error);
        return 2;
    }
    size_t path_size = strlen(argv[2]) + 5;
    char *temporary = malloc(path_size);
    if (!temporary) { fputs("output path allocation failed\n", stderr); return 2; }
    snprintf(temporary, path_size, "%s.tmp", argv[2]);
    FILE *out = fopen(temporary, "wb");
    if (!out) { perror("output"); free(temporary); return 2; }
    emit(out, &program);
    if (fclose(out)) {
        perror("output");
        remove(temporary);
        free(temporary);
        return 2;
    }
    if (same_file(temporary, argv[2])) {
        remove(temporary);
        free(temporary);
        return 0;
    }
    if (rename(temporary, argv[2])) {
        perror("output");
        remove(temporary);
        free(temporary);
        return 2;
    }
    free(temporary);
    return 0;
}
