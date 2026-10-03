#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

struct Sample {
    char tag;
    int left;
    short right;
};

NOINLINE long add_two(long a, long b)
{
    return a + b;
}

NOINLINE int array_sum(const int *array, size_t length, int64_t *out)
{
    if (out == NULL || (array == NULL && length != 0U)) {
        return -1;
    }

    int64_t total = 0;
    for (size_t i = 0; i < length; ++i) {
        total += array[i];
    }

    *out = total;
    return 0;
}

NOINLINE int struct_field_sum(const struct Sample *sample, int *out)
{
    if (sample == NULL || out == NULL) {
        return -1;
    }

    *out = sample->left + sample->right;
    return 0;
}

NOINLINE uint32_t bit_transform(uint32_t value, unsigned int shift)
{
    const unsigned int safe_shift = shift & 31U;
    const uint32_t low_byte = value & UINT32_C(0xFF);
    const uint32_t shifted = value >> safe_shift;

    return (low_byte << 8U) | (shifted & UINT32_C(0xFF));
}

NOINLINE int pointer_read(const int *pointer, int *out)
{
    if (pointer == NULL || out == NULL) {
        return -1;
    }

    *out = *pointer;
    return 0;
}

int main(void)
{
    const int values[] = {3, -2, 7, 10};
    const struct Sample sample = {
        .tag = 'A',
        .left = 40,
        .right = 2
    };
    const int target = 123;

    int64_t array_result = 0;
    int field_result = 0;
    int pointed_result = 0;

    if (array_sum(values, sizeof values / sizeof values[0], &array_result) != 0) {
        fputs("array_sum: invalid argument\n", stderr);
        return EXIT_FAILURE;
    }

    if (struct_field_sum(&sample, &field_result) != 0) {
        fputs("struct_field_sum: invalid argument\n", stderr);
        return EXIT_FAILURE;
    }

    if (pointer_read(&target, &pointed_result) != 0) {
        fputs("pointer_read: invalid argument\n", stderr);
        return EXIT_FAILURE;
    }

    const long add_result = add_two(11L, 31L);
    const uint32_t bit_result = bit_transform(UINT32_C(0x12345678), 4U);

    if (printf("add_two = %ld\n", add_result) < 0 ||
        printf("array_sum = %" PRId64 "\n", array_result) < 0 ||
        printf("struct_field_sum = %d\n", field_result) < 0 ||
        printf("bit_transform = 0x%08" PRIx32 "\n", bit_result) < 0 ||
        printf("pointer_read = %d\n", pointed_result) < 0 ||
        printf("sizeof(struct Sample) = %zu\n", sizeof(struct Sample)) < 0 ||
        printf("_Alignof(struct Sample) = %zu\n", _Alignof(struct Sample)) < 0 ||
        printf("offsetof(tag) = %zu, offsetof(left) = %zu, offsetof(right) = %zu\n",
               offsetof(struct Sample, tag),
               offsetof(struct Sample, left),
               offsetof(struct Sample, right)) < 0) {
        fputs("output error\n", stderr);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
